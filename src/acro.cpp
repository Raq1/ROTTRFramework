#include "acro.h"
#include "cutscene_viewer.h"
#include "brawl.h"
#include "combat_air.h"
#include "game_build.h"
#include "log.h"
#include "memory_probe.h"
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

// How the game resolves a clip (retail, see re\avengers\README.txt and the memory notes):
//   AnimFragmentNode activation -> GetKeylist(key, index) 0x1146D0: registry [0x10FB790] hash node
//   {next, u32 key +8, dtp::AnimLibrary* +0x10}; library +0x10 u32 count, +0x18 AnimEntry[count]
//   (0x18 each: i16 animId, i16 animNum, AnimFxList* +8, char* name +0x10); animId != 0 and
//   animNum < 0x7D00 -> animByNumber[animNum] (0x15CFD50, one pointer per id; refcounts u16 at 0x160E5E0).
// This module loads its own clips, puts them into free ids of those two tables, gives Lara's library an
// extended copy of its entry array (original entries + one new entry per clip) and points the combat evade
// entries at the new clips. The original array stays untouched and is put back when the feature is off.
//
// Roll chain (movement roll, not aiming). Primary SC agent: running + roll -> [11] ground_roll_entry -> [14]
// ground_evade (anim goal Ground_Evade: graph "Evade" plays cover_enter_scramble_* = library 459..463); roll again
// after >= 18 frames (tuning 550, input window 8 frames) -> T327 -> [3] ground_evade_dive (goal Ground_Evade_Dive:
// "Dive" plays 465 ground_roll_front). T338 "ground_evade_dive_to_evade_dive_repeat" (same conditions plus
// Development_Prototype 38, false in retail) is the only way on, so the game stops after two moves. The chain
// sets that condition's NOT bit and points T338 at [14], so the states alternate [14] <-> [3] (each change of
// goal starts the other graph state's fragment again), and the DoTransition hook writes the next Black Widow
// clip into 459..463 / 465 right before each of those states starts. Outside the chain the entries are the
// game's own again (cover entries use the scrambles too).
//
// Acrobatic climb-up: a ledge hang's climb-up ends in [330] ledge_pullup_jump (anim goal "LedgeClimbUp": graph
// 267234 entry_from_ledge / pullup_normal blends 85 ledgeb_jump_up_to_stand_fast, 84 _angleUp, 83 _angleDown by
// ground angle). When the DoTransition hook sees Lara's primary agent enter it with the jump button held, those
// entries play the climb-wheel clips (acro\climb.ini [Climb] <entry name>=<clip file>, re\acro\climbwheel.js), with
// the entries' own fx retimed through each clip's fxmap; leaving the state puts the game's clips back. On a ledge
// Lara can climb onto, the jump press itself starts the game's climb-up (T598 ledge_to_pullup on Requested 111), so a tap
// and a hold look alike at first: HoldClimbTransition makes the entry wait (ClimbHoldMs) until A is let go (the game's
// climb-up) or held long enough (the wheel).
//
// Black Widow high hurdle (v0.9.242): Lara climbs onto a high ledge from the ground in two states, [556]
// hurdle_attach_from_ground_high (graph 267229 Hurdle / Attach / HighWall blends 809 hurdle_hand_onto_short_start and 797
// hurdle_hand_onto_start by the ledge height, 135 -> 165 cm, with an AnimTarget to the markup target) and, when that
// clip ends, [568] hurdle_onto (267234 Hurdle_Onto / FromHigh: 799 hurdle_hand_onto_end, 798 _end_crouch crouched).
// When the DoTransition hook sees Lara's primary agent enter [556], those four entries play Black Widow's jump onto a
// platform (acro\hurdle.ini, re\acro\hurdlebw.js: BW 1072, a step on the wall, both hands on the edge, a tucked vault,
// the markup bone on the edge like the game's clips), each with its own fx retimed through the clip's fxmap; leaving
// hurdle_onto (or the attach ending another way) puts the game's clips back. 809 is also the low blend of
// HighWall_fromWade (out of wading water), which keeps the game's clip because the swap only follows [556].
namespace {

constexpr std::uintptr_t kRvaResolveAnimation = 0x001BE220ull;  // cdc::ANITRACKER_ResolveAnimation(id, resource)
constexpr std::uintptr_t kRvaGetKeylistLimit = 0x0011475Aull;   // cmp ecx,7D00h (animNum limit) in GetKeylist
constexpr std::uintptr_t kRvaGetKeylistRegistry = 0x001146DCull; // mov rcx,[AnimLibraryManager registry]
constexpr std::uintptr_t kRvaRefCounts = 0x0160E5E0ull;
constexpr std::uintptr_t kRvaAnimByNumber = 0x015CFD50ull;
constexpr std::uintptr_t kRvaRegistry = 0x010FB790ull;
constexpr std::uint32_t kClipLimit = 0x7D00;

// Bytes 0..0x5A of ANITRACKER_ResolveAnimation: refcount table disp32 at +0x25/+0x31, pointer table at +0x57.
constexpr std::uint8_t kResolveAnimationSig[] = {
    0x48,0x89,0x5c,0x24,0x08,0x48,0x89,0x74,0x24,0x10,0x48,0x89,0x7c,0x24,0x18,0x41,0x56,0x48,0x83,0xec,
    0x20,0x8b,0xd9,0x4c,0x8d,0x35,0xc2,0x1d,0xe4,0xff,0x8b,0xf2,0x45,0x0f,0xb7,0x84,0x5e,0xe0,0xe5,0x60,
    0x01,0x41,0x8d,0x40,0x01,0x66,0x41,0x89,0x84,0x5e,0xe0,0xe5,0x60,0x01,0x66,0x45,0x85,0xc0,0x75,0x27,
    0xe8,0xaf,0x51,0x18,0x00,0x8b,0xd6,0x45,0x33,0xc0,0x48,0x8b,0xc8,0x4c,0x8b,0x08,0x41,0xff,0x91,0x90,
    0x00,0x00,0x00,0x49,0x89,0x84,0xde,0x50,0xfd,0x5c,0x01
};
constexpr std::uint8_t kGetKeylistLimitSig[] = {0x81,0xF9,0x00,0x7D,0x00,0x00};
constexpr std::uint8_t kGetKeylistRegistrySig[] = {0x48,0x8B,0x0D};

// Lara's library (laracroft.drm animlib 11534, "characters\laracroft\laracroft", 1727 entries in the retail data).
constexpr std::uint32_t kLaraLibraryKey = 11534;
constexpr const char* kLaraLibraryName = "characters\\laracroft\\laracroft";

constexpr int kMaxEvadeTargets = 6;
struct ClipDef {
    char file[64];           // in <game>\ROTTRFramework\acro
    char name[64];           // debug name of the new library entry
    char evades[kMaxEvadeTargets][48];  // Lara library entries (debug names) the clip replaces while the evades are on
    int evadeCount;
    bool chain;              // roll chain clip: its root travel follows the "chain move distance" setting
    bool climb;              // acrobatic climb-up clip: replaces entry "target" while the jump button is held
    bool climbTap;           // with climb: replaces it on a climb-up without the jump button held (the vault)
    bool hurdle;             // high hurdle clip: replaces the entries in "evades" while Lara climbs onto a high ledge
    bool strafeCrouch;       // crouched strafe cycle: replaces entry "target" while LB strafe + low cover
    bool jump;               // Black Widow jump clip: replaces the entries in "evades" (each keeps its own fx)
    char target[48];
};
// Evade states of the "ranged_evade" graph (267229), per weapon (Pistol / Shotgun / SMG): Evade_Forward_No_Enemy ->
// 1052 Dodge_Forward, Evade_Back and Evade_Forward (back) -> 1048 Dodge_Center, Evade_Left -> 1050 Dodge_Left,
// Evade_Right -> 1055 Dodge_Right_pistol (pistols) / 1054 Dodge_Right_smg (shotgun, SMG). The evade clips come from
// ROTTRFramework\acro\evades.ini ([Evades] <entry name>=<clip file>, several entries may share one clip), then the
// roll chain clips in chain order from acro\chain.ini ([Chain] 1=<file> 2=<file> ...); "Reload clips" reads both
// again, and without a file the built-in lists below are used.
constexpr std::size_t kMaxClips = 40;  // evades, chain, climb (wheel + vault), hurdle, crouched strafe
constexpr const char* kDefaultEvades[][2] = {
    {"Dodge_Forward", "acro_frontflip.tr10anim"},         {"Dodge_Center", "acro_backflip.tr10anim"},
    {"Dodge_Left", "acro_side_left_flip.tr10anim"},       {"Dodge_Right_pistol", "acro_side_right_flip.tr10anim"},
    {"Dodge_Right_smg", "acro_side_right_flip.tr10anim"},
};
constexpr const char* kDefaultChain[] = {"acro_chain_diveroll.tr10anim", "acro_chain_revflip.tr10anim",
                                         "acro_chain_twist.tr10anim", "acro_chain_aerialtwist.tr10anim"};
constexpr const char* kDefaultClimb[][2] = {
    {"ledgeb_jump_up_to_stand_fast", "acro_climb_wheel.tr10anim"},
    {"ledgeb_jump_up_to_stand_fast_angleUp", "acro_climb_wheel_up.tr10anim"},
    {"ledgeb_jump_up_to_stand_fast_angleDown", "acro_climb_wheel_down.tr10anim"},
};
constexpr const char* kStateClimbName = "ledge_pullup_jump";
constexpr const char* kDefaultClimbTap[][2] = {
    {"ledgeb_jump_up_to_stand_fast", "acro_climb_vault.tr10anim"},
    {"ledgeb_jump_up_to_stand_fast_angleUp", "acro_climb_vault_up.tr10anim"},
    {"ledgeb_jump_up_to_stand_fast_angleDown", "acro_climb_vault_down.tr10anim"},
};
constexpr const char* kDefaultHurdle[][2] = {
    {"hurdle_hand_onto_start", "acro_hurdle_bw_attach.tr10anim"},
    {"hurdle_hand_onto_short_start", "acro_hurdle_bw_attach.tr10anim"},
    {"hurdle_hand_onto_end", "acro_hurdle_bw_onto.tr10anim"},
    {"hurdle_hand_onto_end_crouch", "acro_hurdle_bw_onto.tr10anim"},
};
constexpr const char* kDefaultStrafeCrouch[][2] = {
    {"ground_run", "acro_crouch_run_forward.tr10anim"},
    {"ground_OM_run_backward", "acro_crouch_run_backward.tr10anim"},
    {"ground_OM_run_right", "acro_crouch_run_right.tr10anim"},
    {"ground_OM_run_left", "acro_crouch_run_left.tr10anim"},
    {"ground_OM_run_right_back", "acro_crouch_run_right_back.tr10anim"},
    {"ground_OM_run_left_back", "acro_crouch_run_left_back.tr10anim"},
    {"ground_walk", "acro_crouch_walk_forward.tr10anim"},
    {"ground_OM_walk_backward", "acro_crouch_walk_backward.tr10anim"},
    {"ground_OM_walk_left", "acro_crouch_walk_left.tr10anim"},
    {"ground_OM_walk_right", "acro_crouch_walk_right.tr10anim"},
    {"ground_OM_walk_left_back", "acro_crouch_walk_left_back.tr10anim"},
    {"ground_OM_walk_right_back", "acro_crouch_walk_right_back.tr10anim"},
    {"ground_idle", "acro_crouch_idle.tr10anim"},
};
// Black Widow jump (v0.9.274, user: "change the jump to Black Widow's"): ROTTR's jump is the takeoff on the ground, then
// physics moves Lara while the in-air graph (267240 InAir_Rise / InAir_Generic_Fall) plays a rise and a fall loop; the
// clips (re\acro\bw2lara.js --root-z) are pieces of BW's running leap 54209 (takeoff 1..5, rise 5..13, fall 15..19 as a
// loop) and her tucked standing jump 62406 (takeoff 0..3, rise 3..14), with the root kept at 0 and, in the air, the
// pelvis 95 cm above it like Lara's own in-air clips. acro\jump.ini [Jump] <library entry>=<clip file>; the landings stay
// the game's. Each entry keeps its own fx (sounds, foot / critical-section alarms).
constexpr const char* kDefaultJump[][2] = {
    {"ground_run_jump_forward", "acro_jump_run_takeoff.tr10anim"},
    {"ground_sprint_jump", "acro_jump_run_takeoff.tr10anim"},
    {"inair_generic_neutral", "acro_jump_run_rise.tr10anim"},
    {"inair_generic_back", "acro_jump_run_rise.tr10anim"},
    {"inair_generic_left", "acro_jump_run_rise.tr10anim"},
    {"inair_generic_right", "acro_jump_run_rise.tr10anim"},
    {"inair_sprint_neutral", "acro_jump_run_rise.tr10anim"},
    {"inair_generic_fall_neutral", "acro_jump_fall.tr10anim"},
    {"inair_generic_fall_back", "acro_jump_fall.tr10anim"},
    {"inair_generic_fall_left", "acro_jump_fall.tr10anim"},
    {"inair_generic_fall_right", "acro_jump_fall.tr10anim"},
    {"ground_generic_jump_forward", "acro_jump_stand_takeoff.tr10anim"},
    {"ground_idle_jump_up", "acro_jump_stand_takeoff.tr10anim"},
    {"inair_vertical_neutral", "acro_jump_stand_rise.tr10anim"},
    {"inair_vertical_back", "acro_jump_stand_rise.tr10anim"},
    {"inair_vertical_left", "acro_jump_stand_rise.tr10anim"},
    {"inair_vertical_right", "acro_jump_stand_rise.tr10anim"},
};
constexpr const char* kStateHurdleAttachName = "hurdle_attach_from_ground_high";
constexpr const char* kStateHurdleOntoName = "hurdle_onto";
constexpr std::uint16_t kAlarmCriticalSectionRelease = 23;  // dtp::useranimalarm
ClipDef g_defs[kMaxClips];
std::size_t g_defCount = 0;
std::size_t g_chainOrder[kMaxClips];  // indices into g_defs
std::size_t g_chainLength = 0;

// Lara library entries the roll states play, and the primary agent's states / transition the chain uses.
constexpr std::uint32_t kScrambleFirst = 459, kScrambleLast = 463;  // cover_enter_scramble_* (graph "Evade")
constexpr const char* kScramblePrefix = "cover_enter_scramble_";
constexpr std::uint32_t kRollEntry = 465;                            // graph "Dive"
constexpr const char* kRollEntryName = "ground_roll_front";
constexpr const char* kStateEvadeName = "ground_evade";
constexpr const char* kStateDiveName = "ground_evade_dive";
constexpr const char* kRepeatTransitionName = "ground_evade_dive_to_evade_dive_repeat";
constexpr const char* kEvadeToDiveName = "ground_evade_to_evade_dive";
constexpr std::size_t kAgentFramesInState = 0x618;  // float, reset by DoTransition, read by Frames_InState
constexpr std::uint16_t kCondDevelopmentPrototype = 791;  // dtp::EnumSCCondProperty
constexpr std::int32_t kPrototypeRollRepeat = 38;
// Retail layouts (see combat_air.cpp): SCAgent +0x08 dtp::SCAgentData*, +0x1C10 current dtp::SCStateData*;
// SCAgentData +0x18 u16 states, +0x1A u16 transitions, +0x20 state refs, +0x28 transition refs;
// SCStateData +0x38 debug name; SCTransitionData +0 u16 target, +4 u16 conditions, +0x10 conditions, +0x18 name;
// SCConditionData 0x28: +0 context, +8 u16 property, +0xC value, +0x10 bit 0 NOT.
constexpr std::size_t kAgentData = 0x08, kAgentCurrentState = 0x1C10;
constexpr std::size_t kAgentDataNumStates = 0x18, kAgentDataNumTransitions = 0x1A;
constexpr std::size_t kAgentDataStates = 0x20, kAgentDataTransitions = 0x28;
constexpr std::size_t kStateName = 0x38;
constexpr std::size_t kTransitionTarget = 0x00, kTransitionNumConditions = 0x04, kTransitionConditions = 0x10;
constexpr std::size_t kTransitionName = 0x18;
constexpr std::size_t kConditionProperty = 0x08, kConditionValue = 0x0C, kConditionFlags = 0x10;
// SCStateData +0x04 u16 property count, +0x10 SCPropertyData[] (0x20 each: +0 u16 id, +2 i16 type, +8 value).
constexpr std::size_t kStateNumProperties = 0x04, kStateProperties = 0x10, kPropertySize = 0x20;
constexpr std::uint16_t kPropSpeedMin = 210, kPropSpeedMax = 212;  // dtp::EnumSCStateProperty Movement_SpeedMin / Max
constexpr std::int16_t kPropTypeFloat = 32;

// Flip sounds are copied from Lara's own fx lists (entries found by debug name, first match wins): the evade's
// early "whoosh" sounds, a standing jump's takeoff and a jump landing, retimed to the clip's takeoff / landing frames.
constexpr const char* kTakeoffSources[] = {"ground_idle_jump_up", "ground_run_jump_forward", "ground_generic_jump_forward"};
constexpr const char* kLandingSources[] = {"ground_generic_land_forward", "ground_run_land"};

// cdc::AnimFxList (an AnimEntry's fx pointer): { dtp::FXLib* effectLib; AnimFxHeader* pTriggers }. Triggers are
// AnimFxHeader { u16 info; i16 keyframeID; u8 fxID; u8 pad[3] } + payload, ended by info 0xFFFF. info bits 0-1 kind
// (0 sound, 1 effect, 2 user alarm, 3 camera), bit 2 disabled, bits 10-11 phase (0 keyframe-timed, 1 fragment
// enter, 2 fragment exit), bits 12-15 payload dwords n (size 8 + align8(4n + 4)). Sound payload: i8 type (0 sound
// plex, 1/3 bark), u8 flags, u8 position mode, pad, u16 bone id, pad, dtp::SoundPlex* at +8. Read by
// cdc::ANIMFX_TriggerEffectsV2 (retail 0x3CDE30) with the fragment's frame window [t0, t1).
struct AnimFxList {
    void* effectLib;
    std::uint8_t* triggers;
};
constexpr std::size_t kFxBufferSize = 2048;

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

struct Clip {
    std::uint8_t* data = nullptr;  // relocated resource body (runtime layout)
    std::uint32_t size = 0;
    int id = -1;                   // animation id / number it is registered under
    std::uint16_t frames = 0;
    std::uint8_t bones = 0;
    bool failed = false;
    int takeoff = -1;              // last ground contact before the airborne part (from <clip>.acro.ini)
    int land = -1;                 // first ground contact after it
    bool roll = false;             // mode=roll: no airborne part, sounds = Lara's own roll, retimed to the length
    float travel = 0.0f;           // root travel over the clip in cm (after the chain distance scale)
    int ready = -1;                // first frame the next roll-chain move may start (landed / back on its feet)
    int fxmapCount = 0;            // climb clips: the replaced entry's keyframe -> this clip's keyframe (piecewise linear)
    int fxmapFrom[8] = {};
    int fxmapTo[8] = {};
};

std::uintptr_t g_base = 0;
bool g_available = false;
std::uint16_t* g_refCounts = nullptr;
void** g_animByNumber = nullptr;
std::uint8_t* const* g_registry = nullptr;

std::atomic_bool g_enabled{false};    // Black Widow flips on the combat evades
std::atomic_bool g_rollChain{false};   // Black Widow acrobatics chain on the movement roll
std::atomic_bool g_flipSounds{true};
std::atomic_bool g_climbWheel{false};  // acrobatic climb-up while the jump button is held
std::atomic_bool g_bwHurdle{false};    // Black Widow's jump onto high ledges (the high hurdle)
std::atomic_bool g_climbVault{false};  // Black Widow's vault on a climb-up from a hang without the jump button held
std::atomic_bool g_bwJump{false};      // Black Widow's jump (takeoff, rise, fall) instead of Lara's
std::atomic_bool g_strafeCrouch{true};  // the LB strafe near low cover stays crouched (crouched omnidirectional cycles)
std::atomic_bool g_strafeCrouchActive{false};  // the crouched cycles are in the library entries now
std::atomic_bool g_strafeTrace{true};   // log what Lara's graph plays while LB strafe is held (v0.9.268 diagnostics)
std::atomic_bool g_noHits{false};       // enemies cannot hit Lara during rolls and combat evades
std::atomic_bool g_lbStrafe{false};     // LB held: strafe (the game's LB heal panel moves to LB+RB)
std::atomic<int> g_climbHoldMs{200};   // how long the jump button must be held (a tap of A starts the game's own climb-up)
std::atomic<ULONGLONG> g_climbHoldSince{0};
std::atomic<int> g_climbHoldLogs{0};
std::atomic<int> g_chainDistance{60};  // % for the roll chain moves: the clips' root travel and the states' push
std::atomic_bool g_reloadRequested{false};
std::atomic_bool g_reapplyRequested{false};
Clip g_clips[kMaxClips];
AnimFxList* g_clipSounds[kMaxClips] = {};   // per clip fx list of the current patch (nullptr: silent)
AnimFxList* g_chainSounds[kMaxClips] = {};  // the same plus the roll's foot IK alarms, for the roll chain
bool g_clipsLoaded = false;
ULONGLONG g_nextTickMs = 0;

// Library patch state (game thread only).
std::uint8_t* g_library = nullptr;       // dtp::AnimLibrary* the patch belongs to
AnimEntry* g_originalEntries = nullptr;
std::uint32_t g_originalCount = 0;
std::atomic<AnimEntry*> g_patchedEntries{nullptr};  // allocated, never freed (readers may still hold it)
bool g_patched = false;
bool g_patchedEvades = false;            // what the current patch was built for
bool g_patchedChain = false;
bool g_patchedJump = false;
int g_jumpRedirected = 0;
int g_redirected = 0;

// Roll chain: SC patch (game thread) and the state the DoTransition hook reads.
struct ChainSc {
    std::uint8_t* agentData = nullptr;
    std::uint8_t* repeat = nullptr;       // T338 SCTransitionData
    std::uint8_t* evadeToDive = nullptr;  // T327 "ground_evade_to_evade_dive"
    // The roll states move Lara on their own (PSCAF_P_Move_AdjustGroundSpeed pushes at Movement_SpeedMin/Max, 8, on top
    // of the clip's root motion; the v0.9.214 log fits 8 cm per frame + root motion): ground_evade 210/212,
    // ground_evade_dive 210/212. Scaled with the chain distance while the chain is on.
    float* speed[4] = {};
    float speedOriginal[4] = {};
    float speedFactor = 1.0f;
    std::uint8_t* repeatCond = nullptr;   // its Development_Prototype condition
    std::uint16_t originalTarget = 0;
    std::uint8_t originalFlags = 0;
};
ChainSc g_chainSc;
bool g_chainEntriesOk = false;           // 459..463 / 465 have the expected names in the current library
std::atomic_bool g_chainArmed{false};    // hook may rewrite the roll entries
std::atomic<void*> g_chainAgent{nullptr};
std::atomic<const std::uint8_t*> g_stateEvade{nullptr};
std::atomic<const std::uint8_t*> g_stateDive{nullptr};
std::atomic<int> g_idxEvade{-1};
std::atomic<int> g_idxDive{-1};
std::atomic<int> g_chainStep{-1};        // -1: not in a chain
std::atomic<int> g_chainLongest{0};
// The clip the current chain move plays.
int g_moveClip = -1;
char g_chainStatus[160] = "off";
// Acrobatic climb-up: the primary agent's pull-up state and whether its entries play our clips now.
std::atomic_bool g_climbArmed{false};
std::atomic<void*> g_climbAgent{nullptr};
const std::uint8_t* g_climbAgentData = nullptr;  // game thread
std::atomic<int> g_idxClimb{-1};
std::atomic<const std::uint8_t*> g_stateClimb{nullptr};
std::atomic_bool g_climbActive{false};
std::atomic<ULONGLONG> g_climbActiveSinceMs{0};
std::atomic<int> g_climbCount{0};
std::atomic<int> g_climbLogs{0};
int g_climbEntry[kMaxClips];  // per clip def: the library entry it replaces, -1
char g_climbStatus[160] = "off";
// high hurdle
std::atomic_bool g_hurdleArmed{false};
std::atomic<void*> g_hurdleAgent{nullptr};
const std::uint8_t* g_hurdleAgentData = nullptr;  // game thread
std::atomic<int> g_idxHurdleAttach{-1};
std::atomic<int> g_idxHurdleOnto{-1};
std::atomic<const std::uint8_t*> g_stateHurdleAttach{nullptr};
std::atomic<const std::uint8_t*> g_stateHurdleOnto{nullptr};
std::atomic_bool g_hurdleActive{false};
std::atomic<ULONGLONG> g_hurdleActiveSinceMs{0};
std::atomic<int> g_hurdleCount{0};
std::atomic<int> g_hurdleLogs{0};
int g_hurdleEntry[kMaxClips][kMaxEvadeTargets];  // per clip def and target: the library entry it replaces, -1
AnimFxList* g_hurdleFx[kMaxClips][kMaxEvadeTargets] = {};
char g_hurdleStatus[160] = "off";
std::uint32_t g_laraKey = kLaraLibraryKey;  // registry key of Lara's library (found by name if it differs)
ULONGLONG g_nextScanMs = 0;
bool g_keyLogged = false;

// Status for the UI.
std::atomic<int> g_statusClips{0};
std::atomic<int> g_statusFirstId{-1};
std::atomic<int> g_statusLibraryCount{0};
std::atomic_bool g_statusPatched{false};
std::atomic<int> g_statusRedirected{0};
std::atomic<int> g_statusSounds[kMaxClips]{};
char g_statusMessage[160] = "not started";

void SetStatus(const char* text) {
    std::snprintf(g_statusMessage, sizeof(g_statusMessage), "%s", text);
}

const char* GameDir() {
    static char dir[MAX_PATH]{};
    if (dir[0]) return dir;
    GetModuleFileNameA(nullptr, dir, MAX_PATH);
    char* slash = std::strrchr(dir, '\\');
    if (slash) slash[1] = '\0';
    return dir;
}

const char* IniPath() {
    static char path[MAX_PATH]{};
    if (!path[0]) std::snprintf(path, sizeof(path), "%sROTTRFramework.ini", GameDir());
    return path;
}

void LoadSettings() {
    // (v1.0.0) every move of these off by default (BlackWidowEvades already was)
    g_enabled.store(GetPrivateProfileIntA("Acrobatics", "BlackWidowEvades", 0, IniPath()) != 0);
    g_rollChain.store(GetPrivateProfileIntA("Acrobatics", "RollChain", 0, IniPath()) != 0);
    g_flipSounds.store(GetPrivateProfileIntA("Acrobatics", "FlipSounds", 1, IniPath()) != 0);
    g_chainDistance.store(std::clamp(static_cast<int>(GetPrivateProfileIntA("Acrobatics", "ChainDistance", 60, IniPath())), 20, 150));
    g_climbWheel.store(GetPrivateProfileIntA("Acrobatics", "ClimbWheel", 0, IniPath()) != 0);
    g_bwHurdle.store(GetPrivateProfileIntA("Acrobatics", "BlackWidowHurdle", 0, IniPath()) != 0);
    g_climbVault.store(GetPrivateProfileIntA("Acrobatics", "ClimbVault", 0, IniPath()) != 0);
    g_bwJump.store(GetPrivateProfileIntA("Acrobatics", "BlackWidowJump", 0, IniPath()) != 0);
    g_strafeCrouch.store(GetPrivateProfileIntA("Acrobatics", "StrafeCrouch", 1, IniPath()) != 0);
    g_strafeTrace.store(GetPrivateProfileIntA("Acrobatics", "StrafeTrace", 1, IniPath()) != 0);
    g_noHits.store(GetPrivateProfileIntA("Acrobatics", "NoHitsInAcrobatics", 0, IniPath()) != 0);
    g_lbStrafe.store(GetPrivateProfileIntA("Acrobatics", "LBStrafe", 0, IniPath()) != 0);
    g_climbHoldMs.store(std::clamp(static_cast<int>(GetPrivateProfileIntA("Acrobatics", "ClimbHoldMs", 200, IniPath())), 60, 1000));
}

void SaveSettings() {
    WritePrivateProfileStringA("Acrobatics", "BlackWidowEvades", g_enabled.load() ? "1" : "0", IniPath());
    WritePrivateProfileStringA("Acrobatics", "RollChain", g_rollChain.load() ? "1" : "0", IniPath());
    WritePrivateProfileStringA("Acrobatics", "FlipSounds", g_flipSounds.load() ? "1" : "0", IniPath());
    char distance[16];
    std::snprintf(distance, sizeof(distance), "%d", g_chainDistance.load());
    WritePrivateProfileStringA("Acrobatics", "ChainDistance", distance, IniPath());
    WritePrivateProfileStringA("Acrobatics", "ClimbWheel", g_climbWheel.load() ? "1" : "0", IniPath());
    WritePrivateProfileStringA("Acrobatics", "BlackWidowHurdle", g_bwHurdle.load() ? "1" : "0", IniPath());
    WritePrivateProfileStringA("Acrobatics", "ClimbVault", g_climbVault.load() ? "1" : "0", IniPath());
    WritePrivateProfileStringA("Acrobatics", "BlackWidowJump", g_bwJump.load() ? "1" : "0", IniPath());
    WritePrivateProfileStringA("Acrobatics", "StrafeCrouch", g_strafeCrouch.load() ? "1" : "0", IniPath());
    WritePrivateProfileStringA("Acrobatics", "StrafeTrace", g_strafeTrace.load() ? "1" : "0", IniPath());
    WritePrivateProfileStringA("Acrobatics", "NoHitsInAcrobatics", g_noHits.load() ? "1" : "0", IniPath());
    WritePrivateProfileStringA("Acrobatics", "LBStrafe", g_lbStrafe.load() ? "1" : "0", IniPath());
    char holdMs[16];
    std::snprintf(holdMs, sizeof(holdMs), "%d", g_climbHoldMs.load());
    WritePrivateProfileStringA("Acrobatics", "ClimbHoldMs", holdMs, IniPath());
}

// <clip>.acro.ini (written by re\acro\bw2lara.js next to the clip): [Clip] mode (jump / roll) and the takeoff /
// land frames for the sounds.
void LoadClipTiming(const char* file, Clip& clip) {
    char path[MAX_PATH];
    std::snprintf(path, sizeof(path), "%sROTTRFramework\\acro\\%s", GameDir(), file);
    char* ext = std::strrchr(path, '.');
    if (!ext || static_cast<std::size_t>(ext - path) + 10 >= sizeof(path)) return;
    std::memcpy(ext, ".acro.ini", sizeof(".acro.ini"));
    char mode[16]{};
    GetPrivateProfileStringA("Clip", "mode", "jump", mode, sizeof(mode), path);
    clip.ready = static_cast<int>(GetPrivateProfileIntA("Clip", "ready", -1, path));
    clip.roll = _stricmp(mode, "roll") == 0;
    clip.takeoff = static_cast<int>(GetPrivateProfileIntA("Clip", "takeoff", -1, path));
    clip.land = static_cast<int>(GetPrivateProfileIntA("Clip", "land", -1, path));
    char fxmap[160]{};
    GetPrivateProfileStringA("Clip", "fxmap", "", fxmap, sizeof(fxmap), path);
    clip.fxmapCount = 0;
    for (const char* p = fxmap; *p && clip.fxmapCount < 8;) {
        int from = 0, to = 0, used = 0;
        if (std::sscanf(p, "%d:%d%n", &from, &to, &used) != 2 || used <= 0) break;
        if (clip.fxmapCount && from <= clip.fxmapFrom[clip.fxmapCount - 1]) break;  // must rise
        clip.fxmapFrom[clip.fxmapCount] = from;
        clip.fxmapTo[clip.fxmapCount] = to;
        ++clip.fxmapCount;
        p += used;
        if (*p == ',') ++p;
    }
    if (clip.ready < 0 || clip.ready > static_cast<int>(clip.frames) - 3)
        clip.ready = std::min(static_cast<int>(clip.frames) - 3, clip.land >= 0 ? clip.land + 2 : clip.frames * 6 / 10);
    if (clip.roll) {
        clip.takeoff = clip.land = -1;
        return;
    }
    if (clip.takeoff < 0 || clip.land <= clip.takeoff || clip.land >= clip.frames) {
        Log::Detailf("Acro: %s has no usable takeoff/landing frames (%d/%d in %s); the flip plays without sounds", file,
                    clip.takeoff, clip.land, path);
        clip.takeoff = clip.land = -1;
    }
}

// Reads a ROTTR resource file (RefDefinitions + body), copies the body into fresh memory and resolves its
// internal pointers. Only internal references are supported (our converter writes nothing else).
bool LoadClip(const char* file, Clip& clip) {
    char path[MAX_PATH];
    std::snprintf(path, sizeof(path), "%sROTTRFramework\\acro\\%s", GameDir(), file);
    HANDLE h = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        Log::Writef("Acro: clip file %s not found", path);
        return false;
    }
    LARGE_INTEGER size{};
    GetFileSizeEx(h, &size);
    if (size.QuadPart < 0x100 || size.QuadPart > 16 * 1024 * 1024) {
        CloseHandle(h);
        Log::Writef("Acro: clip file %s has an unexpected size (%lld bytes)", path, size.QuadPart);
        return false;
    }
    const auto fileSize = static_cast<DWORD>(size.QuadPart);
    auto* raw = static_cast<std::uint8_t*>(VirtualAlloc(nullptr, fileSize, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    DWORD read = 0;
    const bool ok = raw && ReadFile(h, raw, fileSize, &read, nullptr) && read == fileSize;
    CloseHandle(h);
    if (!ok) {
        if (raw) VirtualFree(raw, 0, MEM_RELEASE);
        Log::Writef("Acro: could not read %s", path);
        return false;
    }
    auto i32 = [&](std::size_t o) { return *reinterpret_cast<const std::int32_t*>(raw + o); };
    const int internalCount = i32(0), wideCount = i32(4), intPatchCount = i32(8), shortPatchCount = i32(12), packedCount = i32(16);
    const std::size_t bodyOffset = 20 + static_cast<std::size_t>(internalCount) * 8;
    if (internalCount <= 0 || internalCount > 64 || wideCount || intPatchCount || shortPatchCount || packedCount ||
        bodyOffset + 0xA8 > fileSize) {
        VirtualFree(raw, 0, MEM_RELEASE);
        Log::Detailf("Acro: %s is not a clip written by the converter (refs %d/%d/%d/%d/%d)", file, internalCount,
                    wideCount, intPatchCount, shortPatchCount, packedCount);
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
    const bool trackRefs = refsOk && *reinterpret_cast<void**>(body + 0x68) && *reinterpret_cast<void**>(body + 0x70) &&
                           *reinterpret_cast<void**>(body + 0x78) && *reinterpret_cast<void**>(body + 0x80);
    if (!trackRefs) {
        VirtualFree(body, 0, MEM_RELEASE);
        Log::Writef("Acro: %s has bad pointer data", file);
        return false;
    }
    clip.data = body;
    clip.size = bodySize;
    clip.frames = *reinterpret_cast<const std::uint16_t*>(body + 0x32);
    clip.bones = body[0x37];
    return true;
}

// Registers a loaded clip under a free animation id (searched from the top of the 32000-entry tables, which
// the game's own data leaves unused). Returns the id or -1.
int RegisterClip(Clip& clip, int searchFrom) {
#if defined(_MSC_VER)
    __try {
#endif
        for (int id = searchFrom; id > 16000; --id) {
            if (g_refCounts[id] != 0 || g_animByNumber[id] != nullptr) continue;
            *reinterpret_cast<std::uint16_t*>(clip.data + 0x30) = static_cast<std::uint16_t>(id);
            g_animByNumber[id] = clip.data;
            MemoryBarrier();
            g_refCounts[id] = 1;
            return id;
        }
        return -1;
#if defined(_MSC_VER)
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
#endif
}

// Root travel of a converted clip (layout of re\acro\bw2lara.js): flags = u16 channel-kind mask + per bone 1 byte
// (2 when the first has bit 7: low | 0x80, high), bit c = channel c (rot 0..2, pos 4..6, scale 8..10); meta 4 bytes per
// baked channel, 2 per constant; values = u32 0, then frames floats per baked / 1 float per constant channel. The root
// (global id 1) x/y translation is the root motion: scaled by factor (1 = unchanged). Returns the travel in cm.
float ScaleRootTravelUnsafe(Clip& clip, float factor) {
    std::uint8_t* body = clip.data;
    const auto* ids = *reinterpret_cast<const std::uint16_t* const*>(body + 0x68);
    const auto* flags = *reinterpret_cast<const std::uint8_t* const*>(body + 0x70);
    const auto* meta = *reinterpret_cast<const std::uint8_t* const*>(body + 0x78);
    auto* values = *reinterpret_cast<std::uint8_t* const*>(body + 0x80);
    const int bones = body[0x37], frames = clip.frames;
    const std::uint8_t* fp = flags + 2;
    const std::uint8_t* mp = meta;
    auto* vp = reinterpret_cast<float*>(values + 4);
    float* rootX = nullptr;
    float* rootY = nullptr;
    for (int b = 0; b < bones; ++b) {
        std::uint32_t fl = *fp++;
        if (fl & 0x80) fl = (fl & 0x7Fu) | (static_cast<std::uint32_t>(*fp++) << 8);
        for (int c = 0; c < 12; ++c) {
            if (!(fl & (1u << c))) continue;
            const bool baked = *reinterpret_cast<const std::uint16_t*>(mp) == 0;
            mp += baked ? 4 : 2;
            const int n = baked ? frames : 1;
            if (ids[b] == 1 && baked && c == 4) rootX = vp;
            if (ids[b] == 1 && baked && c == 5) rootY = vp;
            vp += n;
        }
    }
    if (!rootX || !rootY) return 0.0f;
    if (factor != 1.0f) {
        for (int k = 0; k < frames; ++k) {
            rootX[k] *= factor;
            rootY[k] *= factor;
        }
        reinterpret_cast<float*>(body)[6] *= factor;  // header root end translation x (+0x18) / y (+0x1C)
        reinterpret_cast<float*>(body)[7] *= factor;
    }
    return std::hypot(rootX[frames - 1] - rootX[0], rootY[frames - 1] - rootY[0]);
}

void ApplyTravelScale(std::size_t i, Clip& clip) {
    const float factor = g_defs[i].chain ? static_cast<float>(g_chainDistance.load()) / 100.0f : 1.0f;
#if defined(_MSC_VER)
    __try {
#endif
        clip.travel = ScaleRootTravelUnsafe(clip, factor);
#if defined(_MSC_VER)
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        clip.travel = 0.0f;
    }
#endif
}

void SetDef(ClipDef& d, const char* file, const char* name, bool chain) {
    std::snprintf(d.file, sizeof(d.file), "%s", file);
    std::snprintf(d.name, sizeof(d.name), "%s", name);
    d.evadeCount = 0;
    d.chain = chain;
    d.climb = false;
    d.climbTap = false;
    d.hurdle = false;
    d.strafeCrouch = false;
    d.jump = false;
    d.target[0] = '\0';
}

void TrimInPlace(char* text) {
    char* start = text;
    while (*start == ' ' || *start == '\t') ++start;
    if (start != text) std::memmove(text, start, std::strlen(start) + 1);
    for (std::size_t n = std::strlen(text); n && (text[n - 1] == ' ' || text[n - 1] == '\t' || text[n - 1] == '\r'); --n)
        text[n - 1] = '\0';
}

// One clip per file; an entry that names a file already listed joins that clip.
void AddEvade(const char* entryName, const char* file) {
    ClipDef* def = nullptr;
    for (std::size_t i = 0; i < g_defCount && !def; ++i)
        if (!g_defs[i].chain && !_stricmp(g_defs[i].file, file)) def = &g_defs[i];
    if (!def) {
        if (g_defCount >= kMaxClips) return;
        char name[64];
        const char* stem = file;
        if (!_strnicmp(stem, "acro_", 5)) stem += 5;
        std::snprintf(name, sizeof(name), "Acro_Evade_%.48s", stem);
        if (char* dot = std::strrchr(name, '.')) *dot = '\0';
        def = &g_defs[g_defCount++];
        SetDef(*def, file, name, false);
    }
    for (int k = 0; k < def->evadeCount; ++k)
        if (!_stricmp(def->evades[k], entryName)) return;
    if (def->evadeCount < kMaxEvadeTargets)
        std::snprintf(def->evades[def->evadeCount++], sizeof(def->evades[0]), "%s", entryName);
}

// The evade clips from acro\evades.ini (or the built-in list), then the roll chain from acro\chain.ini (or the
// built-in order).
void LoadClipDefs() {
    g_defCount = 0;
    g_chainLength = 0;
    char evadesIni[MAX_PATH];
    std::snprintf(evadesIni, sizeof(evadesIni), "%sROTTRFramework\\acro\\evades.ini", GameDir());
    const bool haveEvadesIni = GetFileAttributesA(evadesIni) != INVALID_FILE_ATTRIBUTES;
    if (haveEvadesIni) {
        static char section[4096];
        const DWORD n = GetPrivateProfileSectionA("Evades", section, sizeof(section), evadesIni);
        for (const char* line = section; n && *line; line += std::strlen(line) + 1) {
            char key[64] = "", value[64] = "";
            const char* eq = std::strchr(line, '=');
            if (!eq || *line == ';' || *line == '#') continue;
            std::snprintf(key, sizeof(key), "%.*s", static_cast<int>(eq - line), line);
            std::snprintf(value, sizeof(value), "%s", eq + 1);
            if (char* comment = std::strchr(value, ';')) *comment = '\0';
            TrimInPlace(key);
            TrimInPlace(value);
            if (key[0] && value[0]) AddEvade(key, value);
        }
    } else {
        for (const auto& e : kDefaultEvades) AddEvade(e[0], e[1]);
    }
    const std::size_t evadeClips = g_defCount;
    for (std::size_t i = 0; i < evadeClips; ++i) {
        char list[256] = "";
        for (int k = 0; k < g_defs[i].evadeCount; ++k)
            std::snprintf(list + std::strlen(list), sizeof(list) - std::strlen(list), "%s%s", k ? ", " : "", g_defs[i].evades[k]);
        Log::Detailf("Acro: evade clip %s for %s (%s)", g_defs[i].file, list, haveEvadesIni ? "acro\\evades.ini" : "built-in list");
    }
    char ini[MAX_PATH];
    std::snprintf(ini, sizeof(ini), "%sROTTRFramework\\acro\\chain.ini", GameDir());
    const bool haveIni = GetFileAttributesA(ini) != INVALID_FILE_ATTRIBUTES;
    for (int n = 1; g_defCount < kMaxClips; ++n) {
        char key[8], file[64] = "";
        std::snprintf(key, sizeof(key), "%d", n);
        if (haveIni) GetPrivateProfileStringA("Chain", key, "", file, sizeof(file), ini);
        else if (static_cast<std::size_t>(n) <= sizeof(kDefaultChain) / sizeof(kDefaultChain[0])) std::snprintf(file, sizeof(file), "%s", kDefaultChain[n - 1]);
        if (!file[0]) break;
        char name[64];
        const char* stem = file;
        if (!std::strncmp(stem, "acro_chain_", 11)) stem += 11;
        std::snprintf(name, sizeof(name), "Acro_Chain_%d_%.40s", n, stem);
        if (char* dot = std::strrchr(name, '.')) *dot = '\0';
        g_chainOrder[g_chainLength++] = g_defCount;
        SetDef(g_defs[g_defCount++], file, name, true);
    }
    Log::Detailf("Acro: roll chain order from %s: %zu moves", haveIni ? "acro\\chain.ini" : "the built-in list", g_chainLength);
    // Acrobatic climb-up clips: acro\climb.ini [Climb] <pull-up entry name>=<clip file>, else the built-in three.
    char climbIni[MAX_PATH];
    std::snprintf(climbIni, sizeof(climbIni), "%sROTTRFramework\\acro\\climb.ini", GameDir());
    const bool haveClimbIni = GetFileAttributesA(climbIni) != INVALID_FILE_ATTRIBUTES;
    auto addClimb = [](const char* entry, const char* file, bool tap) {
        if (g_defCount >= kMaxClips) return;
        char name[64];
        const char* stem = file;
        if (!_strnicmp(stem, "acro_", 5)) stem += 5;
        std::snprintf(name, sizeof(name), tap ? "Acro_ClimbTap_%.44s" : "Acro_Climb_%.48s", stem);
        if (char* dot = std::strrchr(name, '.')) *dot = '\0';
        ClipDef& d = g_defs[g_defCount++];
        SetDef(d, file, name, false);
        d.climb = true;
        d.climbTap = tap;
        std::snprintf(d.target, sizeof(d.target), "%s", entry);
        Log::Detailf("Acro: climb-up clip %s for %s (%s)", file, entry, tap ? "no jump button: the vault" : "jump button held");
    };
    // [Climb]: the clips while the jump button is held (the wheel); [ClimbTap]: every other climb-up (the vault).
    for (int tap = 0; tap < 2; ++tap) {
        int added = 0;
        if (haveClimbIni) {
            static char section[2048];
            const DWORD n = GetPrivateProfileSectionA(tap ? "ClimbTap" : "Climb", section, sizeof(section), climbIni);
            for (const char* line = section; n && *line; line += std::strlen(line) + 1) {
                char key[64] = "", value[64] = "";
                const char* eq = std::strchr(line, '=');
                if (!eq || *line == ';' || *line == '#') continue;
                std::snprintf(key, sizeof(key), "%.*s", static_cast<int>(eq - line), line);
                std::snprintf(value, sizeof(value), "%s", eq + 1);
                if (char* comment = std::strchr(value, ';')) *comment = '\0';
                TrimInPlace(key);
                TrimInPlace(value);
                if (key[0] && value[0]) {
                    addClimb(key, value, tap != 0);
                    ++added;
                }
            }
        }
        if (!added) {
            if (tap) for (const auto& c : kDefaultClimbTap) addClimb(c[0], c[1], true);
            else for (const auto& c : kDefaultClimb) addClimb(c[0], c[1], false);
        }
    }
    // High hurdle clips: acro\hurdle.ini [Hurdle] <entry name>=<clip file> (an entry naming a file already listed joins
    // that clip), else the built-in four entries.
    char hurdleIni[MAX_PATH];
    std::snprintf(hurdleIni, sizeof(hurdleIni), "%sROTTRFramework\\acro\\hurdle.ini", GameDir());
    const bool haveHurdleIni = GetFileAttributesA(hurdleIni) != INVALID_FILE_ATTRIBUTES;
    auto addHurdle = [](const char* entry, const char* file) {
        ClipDef* def = nullptr;
        for (std::size_t i = 0; i < g_defCount && !def; ++i)
            if (g_defs[i].hurdle && !_stricmp(g_defs[i].file, file)) def = &g_defs[i];
        if (!def) {
            if (g_defCount >= kMaxClips) return;
            char name[64];
            const char* stem = file;
            if (!_strnicmp(stem, "acro_", 5)) stem += 5;
            std::snprintf(name, sizeof(name), "Acro_Hurdle_%.48s", stem);
            if (char* dot = std::strrchr(name, '.')) *dot = '\0';
            def = &g_defs[g_defCount++];
            SetDef(*def, file, name, false);
            def->hurdle = true;
        }
        for (int k = 0; k < def->evadeCount; ++k)
            if (!_stricmp(def->evades[k], entry)) return;
        if (def->evadeCount < kMaxEvadeTargets) {
            std::snprintf(def->evades[def->evadeCount++], sizeof(def->evades[0]), "%s", entry);
            Log::Detailf("Acro: high hurdle clip %s for %s", file, entry);
        }
    };
    if (haveHurdleIni) {
        static char section[2048];
        const DWORD n = GetPrivateProfileSectionA("Hurdle", section, sizeof(section), hurdleIni);
        for (const char* line = section; n && *line; line += std::strlen(line) + 1) {
            char key[64] = "", value[64] = "";
            const char* eq = std::strchr(line, '=');
            if (!eq || *line == ';' || *line == '#') continue;
            std::snprintf(key, sizeof(key), "%.*s", static_cast<int>(eq - line), line);
            std::snprintf(value, sizeof(value), "%s", eq + 1);
            if (char* comment = std::strchr(value, ';')) *comment = '\0';
            TrimInPlace(key);
            TrimInPlace(value);
            if (key[0] && value[0]) addHurdle(key, value);
        }
    } else {
        for (const auto& h : kDefaultHurdle) addHurdle(h[0], h[1]);
    }
    // Crouched strafe cycles: acro\strafe.ini [StrafeCrouch] <entry name>=<clip file>, else the built-in thirteen.
    char strafeIni[MAX_PATH];
    std::snprintf(strafeIni, sizeof(strafeIni), "%sROTTRFramework\\acro\\strafe.ini", GameDir());
    auto addStrafeCrouch = [](const char* entry, const char* file) {
        if (g_defCount >= kMaxClips) return;
        char name[64];
        const char* stem = file;
        if (!_strnicmp(stem, "acro_", 5)) stem += 5;
        std::snprintf(name, sizeof(name), "Acro_Strafe_%.48s", stem);
        if (char* dot = std::strrchr(name, '.')) *dot = '\0';
        ClipDef& d = g_defs[g_defCount++];
        SetDef(d, file, name, false);
        d.strafeCrouch = true;
        std::snprintf(d.target, sizeof(d.target), "%s", entry);
    };
    int strafeClips = 0;
    if (GetFileAttributesA(strafeIni) != INVALID_FILE_ATTRIBUTES) {
        static char section[4096];
        const DWORD n = GetPrivateProfileSectionA("StrafeCrouch", section, sizeof(section), strafeIni);
        for (const char* line = section; n && *line; line += std::strlen(line) + 1) {
            char key[64] = "", value[64] = "";
            const char* eq = std::strchr(line, '=');
            if (!eq || *line == ';' || *line == '#') continue;
            std::snprintf(key, sizeof(key), "%.*s", static_cast<int>(eq - line), line);
            std::snprintf(value, sizeof(value), "%s", eq + 1);
            if (char* comment = std::strchr(value, ';')) *comment = '\0';
            TrimInPlace(key);
            TrimInPlace(value);
            if (key[0] && value[0]) {
                addStrafeCrouch(key, value);
                ++strafeClips;
            }
        }
    }
    if (!strafeClips)
        for (const auto& c : kDefaultStrafeCrouch) {
            addStrafeCrouch(c[0], c[1]);
            ++strafeClips;
        }
    Log::Detailf("Acro: crouched strafe: %d cycles (%s)", strafeClips, strafeClips ? "acro\\strafe.ini or built-in" : "none");
    // Black Widow jump: acro\jump.ini [Jump] <library entry>=<clip file> (entries may share a clip), else the built-in list.
    char jumpIni[MAX_PATH];
    std::snprintf(jumpIni, sizeof(jumpIni), "%sROTTRFramework\\acro\\jump.ini", GameDir());
    auto addJump = [](const char* entry, const char* file) {
        ClipDef* def = nullptr;
        for (std::size_t i = 0; i < g_defCount && !def; ++i)
            if (g_defs[i].jump && !_stricmp(g_defs[i].file, file) && g_defs[i].evadeCount < kMaxEvadeTargets) def = &g_defs[i];
        if (!def) {
            if (g_defCount >= kMaxClips) return false;
            char name[64];
            const char* stem = file;
            if (!_strnicmp(stem, "acro_", 5)) stem += 5;
            std::snprintf(name, sizeof(name), "Acro_%.56s", stem);
            if (char* dot = std::strrchr(name, '.')) *dot = '\0';
            def = &g_defs[g_defCount++];
            SetDef(*def, file, name, false);
            def->jump = true;
        }
        for (int k = 0; k < def->evadeCount; ++k)
            if (!_stricmp(def->evades[k], entry)) return false;
        std::snprintf(def->evades[def->evadeCount++], sizeof(def->evades[0]), "%s", entry);
        return true;
    };
    int jumpEntries = 0;
    const bool haveJumpIni = GetFileAttributesA(jumpIni) != INVALID_FILE_ATTRIBUTES;
    if (haveJumpIni) {
        static char section[4096];
        const DWORD n = GetPrivateProfileSectionA("Jump", section, sizeof(section), jumpIni);
        for (const char* line = section; n && *line; line += std::strlen(line) + 1) {
            char key[64] = "", value[64] = "";
            const char* eq = std::strchr(line, '=');
            if (!eq || *line == ';' || *line == '#') continue;
            std::snprintf(key, sizeof(key), "%.*s", static_cast<int>(eq - line), line);
            std::snprintf(value, sizeof(value), "%s", eq + 1);
            if (char* comment = std::strchr(value, ';')) *comment = '\0';
            TrimInPlace(key);
            TrimInPlace(value);
            if (key[0] && value[0] && addJump(key, value)) ++jumpEntries;
        }
    } else {
        for (const auto& j : kDefaultJump)
            if (addJump(j[0], j[1])) ++jumpEntries;
    }
    Log::Detailf("Acro: Black Widow jump: %d library entries (%s)", jumpEntries,
                haveJumpIni ? "acro\\jump.ini" : "built-in list");
}

void EnsureClipsGameThread() {
    if (g_clipsLoaded) return;
    LoadClipDefs();
    g_clipsLoaded = true;  // one attempt per session; file problems are logged
    int loaded = 0, firstId = -1, searchFrom = static_cast<int>(kClipLimit) - 1;
    for (std::size_t i = 0; i < g_defCount; ++i) {
        Clip& clip = g_clips[i];
        if (!LoadClip(g_defs[i].file, clip)) {
            clip.failed = true;
            continue;
        }
        clip.id = RegisterClip(clip, searchFrom);
        if (clip.id < 0) {
            clip.failed = true;
            Log::Writef("Acro: no free animation id for %s", g_defs[i].file);
            continue;
        }
        searchFrom = clip.id - 1;
        if (firstId < 0) firstId = clip.id;
        ++loaded;
        LoadClipTiming(g_defs[i].file, clip);
        ApplyTravelScale(i, clip);
        Log::Detailf("Acro: clip %s loaded (%u frames, %u bones, %u bytes, takeoff %d, landing %d, ready %d, travel %.0f cm%s) "
                    "as animation id %d", g_defs[i].file, clip.frames, clip.bones, clip.size, clip.takeoff, clip.land,
                    clip.ready, clip.travel, g_defs[i].chain ? " at the chain distance" : "", clip.id);
    }
    g_statusClips.store(loaded);
    g_statusFirstId.store(firstId);
}

// Read-only registry lookup (same walk as GetKeylist, without its inserting operator[]).
std::uint8_t* FindLibrary(std::uint32_t key) {
#if defined(_MSC_VER)
    __try {
#endif
        const std::uint8_t* map = *g_registry;
        if (!map) return nullptr;
        const auto* const* buckets = *reinterpret_cast<const std::uint8_t* const* const*>(map);
        const std::uint64_t size = *reinterpret_cast<const std::uint64_t*>(map + 0x10);
        const std::uint64_t bucketCount = *reinterpret_cast<const std::uint64_t*>(map + 0x18);
        if (!buckets || size == 0 || bucketCount == 0) return nullptr;
        const std::uint8_t* node = buckets[static_cast<std::uint64_t>(key) % bucketCount];
        for (int guard = 0; node && guard < 4096; ++guard) {
            if (*reinterpret_cast<const std::uint32_t*>(node + 0x08) == key)
                return *reinterpret_cast<std::uint8_t* const*>(node + 0x10);
            node = *reinterpret_cast<const std::uint8_t* const*>(node);
        }
        return nullptr;
#if defined(_MSC_VER)
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
#endif
}

bool NameEqualsUnsafe(const char* text, const char* expected);

// Fallback when the library is not under the expected key: walk the whole registry (read-only) for the
// library with Lara's debug name. Returns its key and library.
bool ScanRegistryForLara(std::uint32_t& key, std::uint8_t*& library) {
#if defined(_MSC_VER)
    __try {
#endif
        const std::uint8_t* map = *g_registry;
        if (!map) return false;
        const auto* const* buckets = *reinterpret_cast<const std::uint8_t* const* const*>(map);
        const std::uint64_t bucketCount = *reinterpret_cast<const std::uint64_t*>(map + 0x18);
        if (!buckets || bucketCount == 0 || bucketCount > (1u << 20)) return false;
        for (std::uint64_t b = 0; b < bucketCount; ++b) {
            const std::uint8_t* node = buckets[b];
            for (int guard = 0; node && guard < 4096; ++guard) {
                auto* lib = *reinterpret_cast<std::uint8_t* const*>(node + 0x10);
                if (lib) {
                    const char* name = *reinterpret_cast<const char* const*>(lib + 0x20);
                    if (NameEqualsUnsafe(name, kLaraLibraryName)) {
                        key = *reinterpret_cast<const std::uint32_t*>(node + 0x08);
                        library = lib;
                        return true;
                    }
                }
                node = *reinterpret_cast<const std::uint8_t* const*>(node);
            }
        }
        return false;
#if defined(_MSC_VER)
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
#endif
}

// Plain compare, for callers that already run inside __try.
bool NameEqualsUnsafe(const char* text, const char* expected) {
    if (!text) return false;
    for (std::size_t i = 0;; ++i) {
        if (text[i] != expected[i]) return false;
        if (!expected[i]) return true;
        if (i > 256) return false;
    }
}

bool NameEquals(const char* text, const char* expected) {
#if defined(_MSC_VER)
    __try {
#endif
        return text && std::strcmp(text, expected) == 0;
#if defined(_MSC_VER)
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
#endif
}

bool ReadLibrary(std::uint8_t* library, AnimEntry*& entries, std::uint32_t& count, const char*& name) {
#if defined(_MSC_VER)
    __try {
#endif
        count = *reinterpret_cast<const std::uint32_t*>(library + 0x10);
        entries = *reinterpret_cast<AnimEntry* const*>(library + 0x18);
        name = *reinterpret_cast<const char* const*>(library + 0x20);
        return entries && count > 0 && count < 20000;
#if defined(_MSC_VER)
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
#endif
}

void PublishEntries(std::uint8_t* library, AnimEntry* entries, std::uint32_t count, bool growing) {
    // Readers (GetKeylist, any thread) load the count first and the array afterwards, so a growing library gets
    // its new array before the larger count and a shrinking one the smaller count before the old array.
    auto* countSlot = reinterpret_cast<volatile LONG*>(library + 0x10);
    auto* arraySlot = reinterpret_cast<void* volatile*>(library + 0x18);
    if (growing) {
        InterlockedExchangePointer(arraySlot, entries);
        InterlockedExchange(countSlot, static_cast<LONG>(count));
    } else {
        InterlockedExchange(countSlot, static_cast<LONG>(count));
        InterlockedExchangePointer(arraySlot, entries);
    }
}

// ---------- flip sounds ----------
enum class FxGroup { Takeoff, Whoosh, Landing, Roll, FootAlarms };
// dtp::useranimalarm values the foot IK listens to: FootLock/SlideLock/Support/Unlock (49..57), HeelLock/Unlock
// (71..74), LegIKOff/On (80, 81), ToeLock/Unlock (132..135).
bool IsFootAlarm(std::uint16_t alarm) {
    return (alarm >= 49 && alarm <= 57) || (alarm >= 71 && alarm <= 74) || alarm == 80 || alarm == 81 ||
           (alarm >= 132 && alarm <= 135);
}
constexpr const char* kWhooshSource = "Dodge_Forward";  // the clips without an evade of their own use its whoosh

struct FxBuild {
    std::uint8_t buf[kFxBufferSize];
    std::size_t used;
    int count;
    std::uint64_t keys[64];  // what each added trigger plays (dedupe)
    int frames[64];
};

int FindEntryUnsafe(const AnimEntry* entries, std::uint32_t count, const char* name) {
    for (std::uint32_t i = 0; i < count; ++i)
        if (NameEqualsUnsafe(entries[i].name, name)) return static_cast<int>(i);
    return -1;
}

int FindEntry(const AnimEntry* entries, std::uint32_t count, const char* name) {
#if defined(_MSC_VER)
    __try {
#endif
        return FindEntryUnsafe(entries, count, name);
#if defined(_MSC_VER)
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
#endif
}

// Identity of a sound plex for deduplication: the sound id it ends in (every fx list has its own Assignment
// wrapper around a Reference), else the plex address. dtp::SoundPlex { u16 type; ...; void* data +8 }.
std::uint64_t SoundKeyUnsafe(const std::uint8_t* plex) {
    const std::uint8_t* p = plex;
    for (int depth = 0; p && depth < 6; ++depth) {
        const std::uint16_t type = *reinterpret_cast<const std::uint16_t*>(p);
        const std::uint8_t* data = *reinterpret_cast<const std::uint8_t* const*>(p + 8);
        if (!data) break;
        if (type == 1 || type == 2) return 0x100000000ull | *reinterpret_cast<const std::uint32_t*>(data);  // Reference, Derived
        if (type != 6 && type != 7 && type != 11 && type != 14) break;  // Assignment, Envelope, Reaction, Effect wrap m_sound
        p = *reinterpret_cast<const std::uint8_t* const*>(data);
    }
    return reinterpret_cast<std::uintptr_t>(plex);
}

// Appends the wanted triggers of one of Lara's fx lists, retimed to the clip (keyframe-timed, phase 0).
// FxGroup::Roll scales the source's keyframes by rollScale (Lara's roll -> a Black Widow roll of another length).
int AppendTriggersUnsafe(FxBuild& b, const AnimFxList* src, FxGroup group, const Clip& clip, float rollScale = 1.0f) {
    if (!src || !src->triggers) return 0;
    int added = 0;
    const std::uint8_t* t = src->triggers;
    for (int n = 0; n < 64; ++n) {
        const std::uint16_t info = *reinterpret_cast<const std::uint16_t*>(t);
        if (info == 0xFFFF) break;
        const std::size_t size = 8 + ((4u * (info >> 12) + 4u) & ~7u);
        if (size > 64) break;
        const int kind = info & 3, phase = (info >> 10) & 3, keyframe = *reinterpret_cast<const std::int16_t*>(t + 2);
        const int soundType = static_cast<std::int8_t>(t[8]);
        bool take = !(info & 4) && phase != 2 && size >= 24;
        int frame = 0;
        switch (group) {
        case FxGroup::Takeoff:  // a standing jump's push-off: feet, cloth, gear, effort vocal
            take = take && kind == 0;
            frame = clip.takeoff - 1 + (phase == 0 ? keyframe : 0);
            break;
        case FxGroup::Whoosh:  // the evade's own first sounds (air / cloth), without its vocal
            take = take && kind == 0 && soundType == 0 && phase == 0 && keyframe <= 2;
            frame = clip.takeoff - 1 + keyframe;
            break;
        case FxGroup::Landing: {  // feet, impact, gear settling, vocal, dust
            const std::uint16_t effect = *reinterpret_cast<const std::uint16_t*>(t + 8);
            take = take && (kind == 0 || (kind == 1 && effect != 0x1C && effect != 0x1D && effect != 0x1F));
            frame = clip.land + (phase == 0 ? keyframe : 0);
            break;
        }
        case FxGroup::Roll:  // Lara's roll: feet, hand, body thud, cloth (no alarms: they lock states / alert AI)
            take = take && kind == 0;
            frame = phase == 0 ? static_cast<int>(static_cast<float>(keyframe) * rollScale + 0.5f) : 0;
            break;
        case FxGroup::FootAlarms:  // the roll's foot IK alarms (support none + unlock both feet at its start)
            take = take && kind == 2 && IsFootAlarm(*reinterpret_cast<const std::uint16_t*>(t + 8));
            frame = phase == 0 ? static_cast<int>(static_cast<float>(keyframe) * rollScale + 0.5f) : 0;
            break;
        }
        if (take) {
            frame = std::clamp(frame, 0, static_cast<int>(clip.frames) - 1);
            std::uint64_t key = 0;
            if (kind == 0 && soundType == 0) key = SoundKeyUnsafe(*reinterpret_cast<const std::uint8_t* const*>(t + 16));
            else if (kind == 0) key = *reinterpret_cast<const std::uint64_t*>(t + 12) ^ 0x8000000000000000ull;  // bark id, line
            bool duplicate = false;
            for (int k = 0; key && k < b.count; ++k)
                if (b.keys[k] == key && std::abs(b.frames[k] - frame) <= 2) duplicate = true;
            if (!duplicate && b.count < 64 && b.used + size + 8 <= kFxBufferSize) {
                std::uint8_t* out = b.buf + b.used;
                std::memcpy(out, t, size);
                *reinterpret_cast<std::uint16_t*>(out) = static_cast<std::uint16_t>(info & ~0x0C00u);
                *reinterpret_cast<std::int16_t*>(out + 2) = static_cast<std::int16_t>(frame);
                b.keys[b.count] = key;
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

// The roll's own foot alarms (FootSupportNone, FootUnlockLeft / Right at its start) used as templates: support
// none at frame 0 and both feet unlocked on frames 0..6 and every 3rd frame after, so a foot lock the previous
// animation still sends while it blends out (a run step) is undone at once instead of pinning a foot for the move.
int AppendFootUnlocksUnsafe(FxBuild& b, const AnimFxList* roll, const Clip& clip) {
    if (!roll || !roll->triggers) return 0;
    const std::uint8_t* supportNone = nullptr;
    const std::uint8_t* unlockLeft = nullptr;
    const std::uint8_t* unlockRight = nullptr;
    const std::uint8_t* t = roll->triggers;
    for (int n = 0; n < 64; ++n) {
        const std::uint16_t info = *reinterpret_cast<const std::uint16_t*>(t);
        if (info == 0xFFFF) break;
        const std::size_t size = 8 + ((4u * (info >> 12) + 4u) & ~7u);
        if (size > 64) break;
        if ((info & 3) == 2 && !(info & 4) && size == 24) {
            const std::uint16_t alarm = *reinterpret_cast<const std::uint16_t*>(t + 8);
            if (alarm == 54) supportNone = t;
            if (alarm == 56) unlockLeft = t;
            if (alarm == 57) unlockRight = t;
        }
        t += size;
    }
    int added = 0;
    auto put = [&](const std::uint8_t* src, int frame) {
        if (!src || b.count >= 64 || b.used + 24 + 8 > kFxBufferSize) return;
        std::uint8_t* out = b.buf + b.used;
        std::memcpy(out, src, 24);
        *reinterpret_cast<std::uint16_t*>(out) = static_cast<std::uint16_t>(*reinterpret_cast<const std::uint16_t*>(src) & ~0x0C00u);
        *reinterpret_cast<std::int16_t*>(out + 2) = static_cast<std::int16_t>(frame);
        b.keys[b.count] = 0;
        b.frames[b.count] = frame;
        ++b.count;
        b.used += 24;
        ++added;
    };
    put(supportNone, 0);
    for (int frame = 0; frame < static_cast<int>(clip.frames) - 1; frame += frame < 6 ? 1 : 3) {
        put(unlockLeft, frame);
        put(unlockRight, frame);
    }
    return added;
}

// Builds the fx list of clip i out of Lara's original entries; the copied triggers keep pointing at the game's
// sound data (the fx DTPs of Lara's library, resident while the library is). Never freed: graph nodes may hold it.
// sounds: the jump / whoosh / landing (or roll) sounds; footAlarms: the roll's foot IK alarms (support none + unlock
// both feet at the start) that the roll states need, or the foot IK keeps the feet where the previous move left them.
AnimFxList* BuildFxListGameThread(std::size_t i, const AnimEntry* original, std::uint32_t count, bool sounds, bool footAlarms,
                                  int& soundCount) {
    soundCount = 0;
    const Clip& clip = g_clips[i];
    if (!clip.roll && clip.takeoff < 0) sounds = false;
    if (!sounds && !footAlarms) return nullptr;
    static FxBuild b;
    b.used = 0;
    b.count = 0;
    const char* takeoffName = "none";
    const char* landingName = "none";
    const char* whooshName = g_defs[i].evadeCount ? g_defs[i].evades[0] : kWhooshSource;
    int takeoff = 0, whoosh = 0, landing = 0, rollSounds = 0, rollFrames = 0, alarms = 0;
    void* effectLib = nullptr;
#if defined(_MSC_VER)
    __try {
#endif
        // Lara's own roll (graph "Dive" entry); its keyframes are stretched to this clip's length.
        const AnimFxList* roll = nullptr;
        if (kRollEntry < count && NameEqualsUnsafe(original[kRollEntry].name, kRollEntryName) && original[kRollEntry].fx) {
            roll = static_cast<const AnimFxList*>(original[kRollEntry].fx);
            const int num = original[kRollEntry].animNum;
            const auto* anim = num > 0 && static_cast<std::uint32_t>(num) < kClipLimit
                                   ? static_cast<const std::uint8_t*>(g_animByNumber[num]) : nullptr;
            rollFrames = anim ? *reinterpret_cast<const std::uint16_t*>(anim + 0x32) : 0;
            if (rollFrames < 8 || rollFrames > 400) rollFrames = 32;  // retail ground_roll_front: 32 frames
        }
        const float rollScale = rollFrames ? static_cast<float>(clip.frames) / static_cast<float>(rollFrames) : 1.0f;
        if (footAlarms) alarms = AppendFootUnlocksUnsafe(b, roll, clip);
        if (sounds && clip.roll) {
            rollSounds = AppendTriggersUnsafe(b, roll, FxGroup::Roll, clip, rollScale);
            effectLib = roll ? roll->effectLib : nullptr;
        } else if (sounds) {
            const int dodgeEntry = FindEntryUnsafe(original, count, whooshName);  // the evade's own whoosh
            const AnimFxList* dodge = dodgeEntry >= 0 ? static_cast<const AnimFxList*>(original[dodgeEntry].fx) : nullptr;
            const AnimFxList* jump = nullptr;
            const AnimFxList* land = nullptr;
            for (const char* name : kTakeoffSources) {
                const int e = FindEntryUnsafe(original, count, name);
                if (e >= 0 && original[e].fx) {
                    jump = static_cast<const AnimFxList*>(original[e].fx);
                    takeoffName = name;
                    break;
                }
            }
            for (const char* name : kLandingSources) {
                const int e = FindEntryUnsafe(original, count, name);
                if (e >= 0 && original[e].fx) {
                    land = static_cast<const AnimFxList*>(original[e].fx);
                    landingName = name;
                    break;
                }
            }
            takeoff = AppendTriggersUnsafe(b, jump, FxGroup::Takeoff, clip);
            whoosh = AppendTriggersUnsafe(b, dodge, FxGroup::Whoosh, clip);
            landing = AppendTriggersUnsafe(b, land, FxGroup::Landing, clip);
            // Only the landing's dust effect uses the effect library (sounds do not): take the landing list's.
            effectLib = land ? land->effectLib : dodge ? dodge->effectLib : jump ? jump->effectLib : nullptr;
        }
        if (!effectLib && roll) effectLib = roll->effectLib;
#if defined(_MSC_VER)
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        Log::Writef("Acro: reading Lara's fx lists for %s raised an exception; the clip plays without sounds", g_defs[i].file);
        return nullptr;
    }
#endif
    if (!b.count) {
        Log::Detailf("Acro: no sounds found for %s (jump '%s', landing '%s')", g_defs[i].file, takeoffName, landingName);
        return nullptr;
    }
    auto* mem = static_cast<std::uint8_t*>(
        VirtualAlloc(nullptr, sizeof(AnimFxList) + b.used + 8, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    if (!mem) return nullptr;
    auto* list = reinterpret_cast<AnimFxList*>(mem);
    std::uint8_t* triggers = mem + sizeof(AnimFxList);
    std::memcpy(triggers, b.buf, b.used);
    *reinterpret_cast<std::uint16_t*>(triggers + b.used) = 0xFFFF;  // end marker (rest of the 8 bytes stays 0)
    list->effectLib = effectLib;
    list->triggers = triggers;
    soundCount = b.count - alarms;
    if (!sounds)
        Log::Detailf("Acro: %s: %d foot alarms from '%s' (no sounds)", g_defs[i].file, alarms, kRollEntryName);
    else if (clip.roll)
        Log::Detailf("Acro: %s sounds: %d triggers from '%s' (%d frames stretched to %u)%s", g_defs[i].file, rollSounds,
                    kRollEntryName, rollFrames, clip.frames, footAlarms ? ", + roll foot alarms" : "");
    else
        Log::Detailf("Acro: %s sounds: %d triggers (%d from '%s' at frame %d, %d from '%s', %d from '%s' from frame %d)%s",
                    g_defs[i].file, b.count - alarms, takeoff, takeoffName, clip.takeoff - 1, whoosh, whooshName, landing,
                    landingName, clip.land, footAlarms ? ", + roll foot alarms" : "");
    return list;
}

// A climb clip keyframe for keyframe kf of the entry it replaces (the clip's fxmap, piecewise linear).
int MapClimbKeyframe(const Clip& clip, int kf) {
    const int n = clip.fxmapCount;
    if (n < 2) return kf;
    if (kf <= clip.fxmapFrom[0]) return clip.fxmapTo[0] + (kf - clip.fxmapFrom[0]);
    for (int k = 1; k < n; ++k) {
        if (kf > clip.fxmapFrom[k]) continue;
        const int a = clip.fxmapFrom[k - 1], b = clip.fxmapFrom[k];
        const float u = static_cast<float>(kf - a) / static_cast<float>(b - a);
        return static_cast<int>(static_cast<float>(clip.fxmapTo[k - 1]) + u * static_cast<float>(clip.fxmapTo[k] - clip.fxmapTo[k - 1]) + 0.5f);
    }
    return clip.fxmapTo[n - 1] + (kf - clip.fxmapFrom[n - 1]);
}

// The replaced pull-up entry's own fx list on climb clip i: every trigger (sounds, barks, the CriticalSectionLock on
// entry and the CriticalSectionRelease the state waits for before a held stick may end it), keyframe-timed ones moved
// through the fxmap, so the release comes after the landing instead of in the air. Never freed (as the others).
AnimFxList* BuildClimbFxListGameThread(std::size_t i, const AnimEntry* original, int entry, int& soundCount,
                                       const char* entryName = nullptr) {
    soundCount = 0;
    const char* targetName = entryName ? entryName : g_defs[i].target;
    const Clip& clip = g_clips[i];
    static FxBuild b;
    b.used = 0;
    b.count = 0;
    void* effectLib = nullptr;
    int alarms = 0, released = -1;
#if defined(_MSC_VER)
    __try {
#endif
        const auto* src = static_cast<const AnimFxList*>(original[entry].fx);
        if (!src || !src->triggers) return nullptr;
        effectLib = src->effectLib;
        const std::uint8_t* t = src->triggers;
        for (int n = 0; n < 64; ++n) {
            const std::uint16_t info = *reinterpret_cast<const std::uint16_t*>(t);
            if (info == 0xFFFF) break;
            const std::size_t size = 8 + ((4u * (info >> 12) + 4u) & ~7u);
            if (size > 64 || b.count >= 64 || b.used + size + 8 > kFxBufferSize) break;
            std::uint8_t* out = b.buf + b.used;
            std::memcpy(out, t, size);
            const int phase = (info >> 10) & 3;
            if (phase == 0) {
                const int kf = *reinterpret_cast<const std::int16_t*>(t + 2);
                const int frame = std::clamp(MapClimbKeyframe(clip, kf), 0, static_cast<int>(clip.frames) - 1);
                *reinterpret_cast<std::int16_t*>(out + 2) = static_cast<std::int16_t>(frame);
                if ((info & 3) == 2 && size >= 12 && *reinterpret_cast<const std::uint16_t*>(t + 8) == kAlarmCriticalSectionRelease)
                    released = frame;
            }
            if ((info & 3) == 2) ++alarms;
            b.keys[b.count] = 0;
            b.frames[b.count] = 0;
            ++b.count;
            b.used += size;
            t += size;
        }
#if defined(_MSC_VER)
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        Log::Writef("Acro: reading '%s' fx for %s raised an exception; the clip plays without fx", targetName, g_defs[i].file);
        return nullptr;
    }
#endif
    if (!b.count) return nullptr;
    auto* mem = static_cast<std::uint8_t*>(
        VirtualAlloc(nullptr, sizeof(AnimFxList) + b.used + 8, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    if (!mem) return nullptr;
    auto* list = reinterpret_cast<AnimFxList*>(mem);
    std::uint8_t* triggers = mem + sizeof(AnimFxList);
    std::memcpy(triggers, b.buf, b.used);
    *reinterpret_cast<std::uint16_t*>(triggers + b.used) = 0xFFFF;
    list->effectLib = effectLib;
    list->triggers = triggers;
    soundCount = b.count - alarms;
    Log::Detailf("Acro: %s: %d fx of '%s' retimed (%d sounds, %d alarms; critical section released at frame %d of %u)",
                g_defs[i].file, b.count, targetName, b.count - alarms, alarms, released, clip.frames);
    return list;
}

// Writes an entry's clip (animId + animNum as one 32-bit store, then the fx list): the graph may read entries from
// other threads at any time; a reader sees either the old or the new clip.
void WriteEntry(AnimEntry* entry, std::int16_t animId, std::int16_t animNum, void* fx) {
    const std::uint32_t ids = static_cast<std::uint16_t>(animId) | (static_cast<std::uint32_t>(static_cast<std::uint16_t>(animNum)) << 16);
    InterlockedExchange(reinterpret_cast<volatile LONG*>(entry), static_cast<LONG>(ids));
    InterlockedExchangePointer(&entry->fx, fx);
}

// The library entries the roll chain rewrites must be the ones the ground graph plays.
bool CheckChainEntries(const AnimEntry* original, std::uint32_t count) {
    if (kRollEntry >= count || kScrambleLast >= count) return false;
    for (std::uint32_t i = kScrambleFirst; i <= kScrambleLast; ++i) {
        const char* name = original[i].name;
        bool ok = false;
#if defined(_MSC_VER)
        __try {
#endif
            ok = name && std::strncmp(name, kScramblePrefix, std::strlen(kScramblePrefix)) == 0;
#if defined(_MSC_VER)
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            ok = false;
        }
#endif
        if (!ok) return false;
    }
    return NameEquals(original[kRollEntry].name, kRollEntryName);
}

void ApplyPatchGameThread(std::uint8_t* library, AnimEntry* original, std::uint32_t count, bool evades, bool chain,
                          bool jump) {
    std::uint32_t added = 0;
    for (const Clip& clip : g_clips) if (clip.id >= 0) ++added;
    if (!added) return;
    for (std::size_t i = 0; i < g_defCount; ++i) {
        int n = 0;
        bool inChain = false;
        for (std::size_t k = 0; k < g_chainLength; ++k) inChain = inChain || g_chainOrder[k] == i;
        const bool loaded = g_clips[i].id >= 0, sounds = g_flipSounds.load();
        if (g_defs[i].jump) {  // each replaced entry keeps its own fx (set in the redirect below)
            g_climbEntry[i] = -1;
            g_clipSounds[i] = nullptr;
            g_chainSounds[i] = nullptr;
            g_statusSounds[i].store(0);
            continue;
        }
        if (g_defs[i].strafeCrouch) {
            g_climbEntry[i] = loaded ? FindEntry(original, count, g_defs[i].target) : -1;
            if (loaded && g_climbEntry[i] < 0)
                Log::Detailf("Acro: Lara's library has no entry '%s'; %s is not used", g_defs[i].target, g_defs[i].file);
            g_clipSounds[i] = g_climbEntry[i] >= 0 ? static_cast<AnimFxList*>(original[g_climbEntry[i]].fx) : nullptr;  // own footsteps
            g_chainSounds[i] = nullptr;
            g_statusSounds[i].store(0);
            continue;
        }
        if (g_defs[i].hurdle) {
            int total = 0;
            for (int k = 0; k < kMaxEvadeTargets; ++k) {
                const bool named = loaded && k < g_defs[i].evadeCount;
                g_hurdleEntry[i][k] = named ? FindEntry(original, count, g_defs[i].evades[k]) : -1;
                if (named && g_hurdleEntry[i][k] < 0)
                    Log::Detailf("Acro: Lara's library has no entry '%s'; %s is not used for it", g_defs[i].evades[k], g_defs[i].file);
                g_hurdleFx[i][k] = g_hurdleEntry[i][k] >= 0
                                       ? BuildClimbFxListGameThread(i, original, g_hurdleEntry[i][k], n, g_defs[i].evades[k])
                                       : nullptr;
                if (g_hurdleFx[i][k]) total += n;
            }
            g_climbEntry[i] = -1;
            g_clipSounds[i] = nullptr;
            g_chainSounds[i] = nullptr;
            g_statusSounds[i].store(total);
            continue;
        }
        if (g_defs[i].climb) {
            g_climbEntry[i] = loaded ? FindEntry(original, count, g_defs[i].target) : -1;
            if (loaded && g_climbEntry[i] < 0)
                Log::Detailf("Acro: Lara's library has no entry '%s'; %s is not used", g_defs[i].target, g_defs[i].file);
            g_clipSounds[i] = g_climbEntry[i] >= 0 ? BuildClimbFxListGameThread(i, original, g_climbEntry[i], n) : nullptr;
            g_chainSounds[i] = nullptr;
            g_statusSounds[i].store(n);
            continue;
        }
        g_climbEntry[i] = -1;
        g_chainSounds[i] = loaded && chain && inChain ? BuildFxListGameThread(i, original, count, sounds, true, n) : nullptr;
        if (g_defs[i].evadeCount)
            g_clipSounds[i] = loaded ? BuildFxListGameThread(i, original, count, sounds, false, n) : nullptr;
        else
            g_clipSounds[i] = g_chainSounds[i];
        g_statusSounds[i].store(n);
    }
    const std::uint32_t total = count + added;
    auto* entries = static_cast<AnimEntry*>(
        VirtualAlloc(nullptr, sizeof(AnimEntry) * total, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    if (!entries) return;
    std::memcpy(entries, original, sizeof(AnimEntry) * count);
    std::uint32_t slot = count;
    int redirected = 0, jumpRedirected = 0;
    for (std::size_t i = 0; i < g_defCount; ++i) {
        const Clip& clip = g_clips[i];
        if (clip.id < 0) continue;
        AnimEntry& extra = entries[slot++];
        extra.animId = static_cast<std::int16_t>(clip.id);
        extra.animNum = static_cast<std::int16_t>(clip.id);
        extra.pad = 0;
        extra.fx = g_clipSounds[i];
        extra.name = g_defs[i].name;
        if (g_defs[i].hurdle || g_defs[i].strafeCrouch) continue;  // swapped in only while needed
        if (g_defs[i].jump) {
            if (!jump) continue;
            for (int k = 0; k < g_defs[i].evadeCount; ++k) {
                const int e = FindEntry(original, count, g_defs[i].evades[k]);
                if (e < 0) {
                    Log::Detailf("Acro: Lara's library has no entry '%s'; %s is not used for it", g_defs[i].evades[k], g_defs[i].file);
                    continue;
                }
                entries[e].animId = static_cast<std::int16_t>(clip.id);
                entries[e].animNum = static_cast<std::int16_t>(clip.id);
                entries[e].fx = original[e].fx;  // its own sounds and foot / critical-section alarms
                ++jumpRedirected;
            }
            continue;
        }
        if (!evades) continue;
        for (int k = 0; k < g_defs[i].evadeCount; ++k) {
            const int evade = FindEntry(original, count, g_defs[i].evades[k]);
            if (evade >= 0) {
                entries[evade].animId = static_cast<std::int16_t>(clip.id);
                entries[evade].animNum = static_cast<std::int16_t>(clip.id);
                entries[evade].fx = g_clipSounds[i];  // the evade's own list is timed for the old clip; ours is retimed
                ++redirected;
            } else {
                Log::Detailf("Acro: Lara's library has no entry '%s'; %s is not used for it", g_defs[i].evades[k], g_defs[i].file);
            }
        }
    }
    g_chainEntriesOk = chain && CheckChainEntries(original, count);
    if (chain && !g_chainEntriesOk)
        Log::Detailf("Acro: Lara's library entries %u..%u / %u are not the roll scrambles / '%s'; roll chain off",
                    kScrambleFirst, kScrambleLast, kRollEntry, kRollEntryName);
    PublishEntries(library, entries, total, true);
    g_library = library;
    g_originalEntries = original;
    g_originalCount = count;
    g_patchedEntries.store(entries);
    g_patched = true;
    g_patchedEvades = evades;
    g_patchedChain = chain;
    g_patchedJump = jump;
    g_redirected = redirected;
    g_jumpRedirected = jumpRedirected;
    g_chainStep.store(-1);
    Log::Detailf("Acro: Lara's animation library %p extended from %u to %u entries (new entries %u..%u), "
                "%d combat evades redirected to the Black Widow clips, %d jump entries to the Black Widow jump%s",
                library, count, total, count, total - 1, redirected, jumpRedirected,
                g_chainEntriesOk ? ", roll chain entries ok" : "");
}

void RestoreGameThread() {
    g_chainArmed.store(false);
    g_climbArmed.store(false);
    g_climbActive.store(false);
    g_hurdleArmed.store(false);
    g_hurdleActive.store(false);
    g_strafeCrouchActive.store(false);
    if (!g_patched || !g_library) return;
    AnimEntry* current = nullptr;
    std::uint32_t count = 0;
    const char* name = nullptr;
    if (ReadLibrary(g_library, current, count, name) && current == g_patchedEntries.load())
        PublishEntries(g_library, g_originalEntries, g_originalCount, false);
    g_patched = false;
    g_redirected = 0;
    g_chainEntriesOk = false;
    Log::Detail("Acro: Lara's animation library restored");
}

// ---------- roll chain: state control patch ----------
const char* StateNameUnsafe(const std::uint8_t* state) {
    return state ? *reinterpret_cast<const char* const*>(state + kStateName) : nullptr;
}

// The game's own value of a state float we scale, remembered per address: a new agent can reuse state control data we
// already patched (a level reload keeps the DTP), and its current value is then ours, not the game's.
float RememberedOriginal(float* value) {
    static float* addresses[16] = {};
    static float originals[16] = {};
    for (int i = 0; i < 16; ++i) if (addresses[i] == value) return originals[i];
    for (int i = 0; i < 16; ++i) if (!addresses[i]) { addresses[i] = value; originals[i] = *value; return originals[i]; }
    return *value;
}

// Finds the chain's states / transition in Lara's primary agent data and checks every value the patch relies on.
bool FindChainScUnsafe(std::uint8_t* agentData, ChainSc& sc, int& idxEvade, int& idxDive, const std::uint8_t*& evade,
                       const std::uint8_t*& dive, char* why, std::size_t whySize) {
    const std::uint16_t numStates = *reinterpret_cast<const std::uint16_t*>(agentData + kAgentDataNumStates);
    const std::uint16_t numTransitions = *reinterpret_cast<const std::uint16_t*>(agentData + kAgentDataNumTransitions);
    auto* const* states = *reinterpret_cast<std::uint8_t* const* const*>(agentData + kAgentDataStates);
    auto* const* transitions = *reinterpret_cast<std::uint8_t* const* const*>(agentData + kAgentDataTransitions);
    if (!states || !transitions || numStates < 16 || numStates > 8000 || numTransitions > 16000) {
        std::snprintf(why, whySize, "agent data not recognised");
        return false;
    }
    idxEvade = idxDive = -1;
    for (int i = 0; i < numStates && (idxEvade < 0 || idxDive < 0); ++i) {
        const char* name = StateNameUnsafe(states[i]);
        if (!name) continue;
        if (std::strcmp(name, kStateEvadeName) == 0) idxEvade = i;
        else if (std::strcmp(name, kStateDiveName) == 0) idxDive = i;
    }
    if (idxEvade < 0 || idxDive < 0) {
        std::snprintf(why, whySize, "states %s / %s not found", kStateEvadeName, kStateDiveName);
        return false;
    }
    std::uint8_t* repeat = nullptr;
    std::uint8_t* evadeToDive = nullptr;
    for (int i = 0; i < numTransitions && (!repeat || !evadeToDive); ++i) {
        std::uint8_t* t = transitions[i];
        const char* name = t ? *reinterpret_cast<const char* const*>(t + kTransitionName) : nullptr;
        if (!name) continue;
        if (std::strcmp(name, kRepeatTransitionName) == 0) repeat = t;
        else if (std::strcmp(name, kEvadeToDiveName) == 0) evadeToDive = t;
    }
    if (!repeat) {
        std::snprintf(why, whySize, "transition %s not found", kRepeatTransitionName);
        return false;
    }
    const std::uint16_t target = *reinterpret_cast<const std::uint16_t*>(repeat + kTransitionTarget);
    const std::uint16_t numConditions = *reinterpret_cast<const std::uint16_t*>(repeat + kTransitionNumConditions);
    std::uint8_t* cond = *reinterpret_cast<std::uint8_t* const*>(repeat + kTransitionConditions);
    if ((target != idxDive && target != idxEvade) || numConditions < 1 || !cond ||
        *reinterpret_cast<const std::uint16_t*>(cond + kConditionProperty) != kCondDevelopmentPrototype ||
        *reinterpret_cast<const std::int32_t*>(cond + kConditionValue) != kPrototypeRollRepeat) {
        std::snprintf(why, whySize, "transition %s has unexpected data (target %u, %u conditions)", kRepeatTransitionName,
                      target, numConditions);
        return false;
    }
    int found = 0;
    for (int k = 0; k < 2; ++k) {
        const std::uint8_t* state = states[k == 0 ? idxEvade : idxDive];
        const std::uint16_t n = *reinterpret_cast<const std::uint16_t*>(state + kStateNumProperties);
        std::uint8_t* props = *reinterpret_cast<std::uint8_t* const*>(state + kStateProperties);
        for (int p = 0; props && p < n && p < 256; ++p) {
            std::uint8_t* prop = props + p * kPropertySize;
            const std::uint16_t id = *reinterpret_cast<const std::uint16_t*>(prop);
            if (*reinterpret_cast<const std::int16_t*>(prop + 2) != kPropTypeFloat) continue;
            if (id == kPropSpeedMin || id == kPropSpeedMax) {
                const int slot = k * 2 + (id == kPropSpeedMax ? 1 : 0);
                sc.speed[slot] = reinterpret_cast<float*>(prop + 8);
                sc.speedOriginal[slot] = RememberedOriginal(sc.speed[slot]);
                ++found;
            }
        }
    }
    if (found != 4) {
        for (float*& p : sc.speed) p = nullptr;  // the chain works without it, only farther
    }
    sc.agentData = agentData;
    sc.repeat = repeat;
    sc.evadeToDive = evadeToDive;  // may be null: then only the repeat transition waits for the landing
    sc.repeatCond = cond;
    evade = states[idxEvade];
    dive = states[idxDive];
    return true;
}

bool MakeWritable(void* address, std::size_t size) {
    return MemoryProbe::MakeWritable(address, size);   // (VirtualQuery on the heap: milliseconds, so its answers are kept)
}

// T338: condition 0 (Development_Prototype 38) negated -> true in retail; target -> ground_evade.
bool ApplyChainScGameThread(void* agent) {
    if (g_chainSc.agentData) return true;
    char why[160] = "";
    ChainSc sc;
    int idxEvade = -1, idxDive = -1;
    const std::uint8_t* evade = nullptr;
    const std::uint8_t* dive = nullptr;
    bool ok = false;
#if defined(_MSC_VER)
    __try {
#endif
        auto* agentData = *reinterpret_cast<std::uint8_t**>(static_cast<std::uint8_t*>(agent) + kAgentData);
        ok = agentData && FindChainScUnsafe(agentData, sc, idxEvade, idxDive, evade, dive, why, sizeof(why));
        if (ok && !(MakeWritable(sc.repeat, 2) && MakeWritable(sc.repeatCond + kConditionFlags, 1))) {
            std::snprintf(why, sizeof(why), "state control data is not writable");
            ok = false;
        }
        if (ok) {
            sc.originalTarget = static_cast<std::uint16_t>(idxDive);
            sc.originalFlags = static_cast<std::uint8_t>(sc.repeatCond[kConditionFlags] & ~1u);
            sc.repeatCond[kConditionFlags] = static_cast<std::uint8_t>(sc.originalFlags | 1u);
            *reinterpret_cast<std::uint16_t*>(sc.repeat + kTransitionTarget) = static_cast<std::uint16_t>(idxEvade);
            // a speed already scaled by an earlier patch of this data would read as "original": only trust 8-ish values
            for (int k = 0; k < 4; ++k)
                if (sc.speed[k] && (sc.speedOriginal[k] < 1.0f || sc.speedOriginal[k] > 50.0f)) sc.speed[k] = nullptr;
            if (sc.speed[0] && MakeWritable(sc.speed[0], 4) && sc.speed[2] && MakeWritable(sc.speed[2], 4)) {
                sc.speedFactor = static_cast<float>(g_chainDistance.load()) / 100.0f;
                for (int k = 0; k < 4; ++k) if (sc.speed[k]) *sc.speed[k] = sc.speedOriginal[k] * sc.speedFactor;
            } else {
                for (float*& p : sc.speed) p = nullptr;
            }
        }
#if defined(_MSC_VER)
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        std::snprintf(why, sizeof(why), "exception while reading the state control data");
        ok = false;
    }
#endif
    if (!ok) {
        static int logged = 0;
        if (logged++ < 3) Log::Writef("Acro: roll chain unavailable: %s", why);
        std::snprintf(g_chainStatus, sizeof(g_chainStatus), "unavailable: %s", why);
        return false;
    }
    g_chainSc = sc;
    g_idxEvade.store(idxEvade);
    g_idxDive.store(idxDive);
    g_stateEvade.store(evade);
    g_stateDive.store(dive);
    g_chainAgent.store(agent);
    Log::DetailOncef("Acro: roll chain on (agent %p, states %s=%d / %s=%d, %s now -> %s, %zu moves; state push %s %.1f -> %.1f)",
                agent, kStateEvadeName, idxEvade, kStateDiveName, idxDive, kRepeatTransitionName, kStateEvadeName, g_chainLength,
                sc.speed[0] ? "scaled" : "unchanged", sc.speedOriginal[0], sc.speed[0] ? *sc.speed[0] : sc.speedOriginal[0]);
    return true;
}

void RestoreChainScGameThread() {
    g_chainArmed.store(false);
    if (!g_chainSc.agentData) return;
#if defined(_MSC_VER)
    __try {
#endif
        // Only if the data is still the transition we patched (not memory reused after a level unload).
        const char* name = *reinterpret_cast<const char* const*>(g_chainSc.repeat + kTransitionName);
        if (name && std::strcmp(name, kRepeatTransitionName) == 0 &&
            *reinterpret_cast<const std::uint16_t*>(g_chainSc.repeatCond + kConditionProperty) == kCondDevelopmentPrototype) {
            *reinterpret_cast<std::uint16_t*>(g_chainSc.repeat + kTransitionTarget) = g_chainSc.originalTarget;
            g_chainSc.repeatCond[kConditionFlags] = g_chainSc.originalFlags;
            for (int k = 0; k < 4; ++k) if (g_chainSc.speed[k]) *g_chainSc.speed[k] = g_chainSc.speedOriginal[k];
        }
#if defined(_MSC_VER)
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
#endif
    g_chainSc = ChainSc{};
    g_chainAgent.store(nullptr);
    Log::Detail("Acro: roll chain off (state control restored)");
}

// Puts the roll entries back to the game's own clips (outside a chain).
void ResetChainEntries() {
    AnimEntry* entries = g_patchedEntries.load();
    const AnimEntry* original = g_originalEntries;
    if (!entries || !original) return;
    for (std::uint32_t i = kScrambleFirst; i <= kScrambleLast; ++i)
        WriteEntry(&entries[i], original[i].animId, original[i].animNum, original[i].fx);
    WriteEntry(&entries[kRollEntry], original[kRollEntry].animId, original[kRollEntry].animNum, original[kRollEntry].fx);
}

// "Reload clips": put the original library back, forget the loaded clips and let the normal flow load the files
// again under new ids. The old clip memory and ids stay (graph nodes may still hold them); a reload costs a
// few KB and one id per clip.
void ReloadClipsGameThread() {
    if (g_patched) RestoreGameThread();
    for (Clip& clip : g_clips) clip = Clip{};
    g_clipsLoaded = false;
    g_statusClips.store(0);
    g_statusFirstId.store(-1);
    Log::Detail("Acro: reloading the clips from disk");
}

// ---------- LB strafe ----------
// The game's own omnidirectional movement, a prototype Rise never switches on. PSCCF_Move_Omnidirectional (cond 247) is
// "NOT player_movement_omnidirectional_forced_off AND animgraph_allows_omnidirectional_move AND (forced_on OR ...)",
// _Allowed (248) the same without the last part (named values of Lara's instance; the symbols are the game's globals
// read by that function). The SC then takes [66] / [67] ground_omnidirectional_walk / run (Turn_TurnType 25: she keeps
// facing ahead). The move graph (267234 Move_States / Run / NoCover) blends in "Move_Oriented_States" only when the
// named value anim_omnidirectional_movement (symbol 1923) is on, and that graph's entry picks its "Omnidirectional" pipe
// (ground_OM_run_backward / _left / _right / _left_back / _right_back and ground_run, blended by the move-vs-facing
// angle) on Status_Move_Omnidirectional_Allowed. While LB is held: the three named values are on and the primary
// agent's condition table answers 247 and 248 (as before for 247) whatever the game computes.
using XInputGetStateFn = DWORD(WINAPI*)(DWORD, XINPUT_STATE*);
XInputGetStateFn g_origXInput13 = nullptr;
XInputGetStateFn g_origXInput910 = nullptr;
std::atomic_bool g_padHooks{false};
std::atomic_bool g_strafeHeld{false};      // LB alone, not aiming, at the game's last pad read
std::atomic<std::uint32_t> g_strafePresses{0};
std::atomic<std::uint32_t> g_strafeLogs{0};
std::atomic<std::uint32_t> g_padStick{0};  // left stick at the game's last pad read (x low 16 bits, y high), for the strafe trace
std::atomic<WORD> g_padButtons{0};         // its buttons before our LB change
std::atomic<std::uint8_t> g_padLeftTrigger{0};    // (v0.9.582) its left trigger (the aim), for PadAimHeld
std::atomic<ULONGLONG> g_padReadMs{0};
constexpr std::uint16_t kCondMoveOmnidirectional = 247;        // dtp::EnumSCCondProperty Status_Move_Omnidirectional
constexpr std::uint16_t kCondMoveOmnidirectionalAllowed = 248;  // Status_Move_Omnidirectional_Allowed
constexpr std::size_t kAgentConditionTable = 0x1C40, kConditionEntrySize = 0x80, kConditionEntryFunction = 0x08;
constexpr std::size_t kAgentInstance = 0x10;                     // SCAgent::m_instance
constexpr std::size_t kAgentComponent = 0x00, kAgentDataName = 0x50;   // SCAgent::m_pStateControlComponent, SCAgentDebugName
constexpr std::size_t kComponentDtp = 0x2A8, kComponentAgents = 0x2C0;  // StateControlComponent (dtp +8: agent count)
using ConditionFn = bool(__fastcall*)(void* agent, void* memory, const std::uint8_t* condition);
bool __fastcall OmniCondition(void* agent, void* memory, const std::uint8_t* condition);
bool __fastcall OmniAllowedCondition(void* agent, void* memory, const std::uint8_t* condition);
struct OmniOverride {
    std::uint16_t condition;
    ConditionFn ours;
    ConditionFn original;  // the game's function of that table entry
};
OmniOverride g_omniOverrides[2] = {{kCondMoveOmnidirectional, &OmniCondition, nullptr},
                                   {kCondMoveOmnidirectionalAllowed, &OmniAllowedCondition, nullptr}};
std::atomic<void*> g_omniAgent{nullptr};   // the primary agent whose table entries are ours
std::atomic<std::uint32_t> g_omniAnswers{0};

// Named values (retail = TR2 layout): cdc::NamedValue {u32 system (2 = instance), u16 symbol, u64 instance id (+8)};
// the constructor takes the instance and a cdc::Symbol (u16), operator=(bool) stores it in the instance's named value set
// (the anim graph's rtFlag / rtValue inputs read that set). Symbol ids: <game data>\pcx64-w\symbol.ids (re\symbol.ids.txt).
constexpr std::uintptr_t kRvaNamedValueCtor = 0x00811590ull;       // cdc::NamedValue::NamedValue(Instance*, Symbol*)
constexpr std::uintptr_t kRvaNamedValueSetBool = 0x00104C60ull;    // cdc::NamedValue::operator=(bool)
constexpr std::uintptr_t kRvaOmniSymbolCStr = 0x003921A0ull;       // cdc::Symbol::c_str (log only)
constexpr std::uintptr_t kRvaMoveOmniForceOnLoad = 0x0067DA59ull;  // PSCCF_Move_Omnidirectional: movzx eax,[ForceOn symbol]
constexpr std::uintptr_t kRvaMoveOmniAllowsLoad = 0x0067DC03ull;   // same function: movzx eax,[AnimGraphAllows symbol]
constexpr std::uint8_t kNamedValueCtorSig[] = {
    0x40,0x53,0x48,0x83,0xEC,0x20,0x33,0xC0,0x48,0x8B,0xD9,0x66,0x89,0x41,0x04,0x41,0x0F,0xB7,0x00,0x4C,0x8D,0x44,0x24,0x30,
    0x4C,0x8B
};
constexpr std::uint8_t kNamedValueSetBoolSig[] = {
    0x48,0x89,0x5C,0x24,0x08,0x48,0x89,0x6C,0x24,0x10,0x48,0x89,0x74,0x24,0x18,0x57,0x48,0x83,0xEC,0x20,0x8B,0x01,0x0F,0xB6
};
constexpr std::uint8_t kOmniSymbolCStrSig[] = {0x0F,0xB7,0x11,0x48,0x8B,0x0D};
constexpr std::uint8_t kMoveOmniForceOnLoadSig[] = {0x0F,0xB7,0x05,0xAC,0xA3,0x6D,0x02};
constexpr std::uint8_t kMoveOmniAllowsLoadSig[] = {0x0F,0xB7,0x05,0x0A,0xA2,0x6D,0x02};
constexpr std::uint16_t kSymAnimOmnidirectionalMovement = 1923;  // "anim_omnidirectional_movement"
struct NamedValueRef {
    std::uint32_t system;
    std::uint16_t symbol;
    std::uint16_t pad;
    std::uint64_t instance;
};
// The combat run (graph Move_States / Run and Walk -> "Combat" while player_combat_status_apply_combat_run is 1; it has
// no omnidirectional switch). PlayerCombatStatus::SetStatus (retail 0x8500xx) writes that named value only when the
// combat status changes: ForceCombatRun unless it is -1, else 1 for status 1 or 2 (an absent ForceCombatRun counts as 0,
// as there). Reading: cdc::NamedValue::GetNamedValueComponent -> its NamedValueSet (+0x2A8) Get(symbol, false) ->
// cdc::Variant::CastToInt.
constexpr std::uintptr_t kRvaNamedValueSetInt = 0x001F4780ull;     // cdc::NamedValue::operator=(int)
constexpr std::uintptr_t kRvaNamedValueComponent = 0x001147F0ull;  // cdc::NamedValue::GetNamedValueComponent
constexpr std::uintptr_t kRvaNamedValueSetGet = 0x001132D0ull;     // cdc::NamedValueSet::Get(Symbol*, bool create)
constexpr std::uintptr_t kRvaVariantCastToInt = 0x0022DBD0ull;     // cdc::Variant::CastToInt
constexpr std::uintptr_t kRvaCombatRunForcedLoad = 0x008501CFull;  // SetStatus: movzx eax,[ForceCombatRun symbol]
constexpr std::uintptr_t kRvaCombatRunApplyLoad = 0x008502B6ull;   // SetStatus: movzx eax,[ApplyCombatRun symbol]
constexpr std::uintptr_t kRvaCombatStatusLoad = 0x00850328ull;     // SetStatus: movzx eax,[CurrentCombatStatus symbol]
constexpr std::uint8_t kNamedValueSetIntSig[] = {
    0x48,0x89,0x5C,0x24,0x08,0x48,0x89,0x6C,0x24,0x10,0x48,0x89,0x74,0x24,0x18,0x57,0x48,0x83,0xEC,0x20,0x8B,0x01,0x8B,0xF2
};
constexpr std::uint8_t kNamedValueComponentSig[] = {0x48,0x8B,0x51,0x08,0x48,0xB8,0x00,0x00,0x00,0x00,0xFF,0xFF,0xFF,0xFF,0x48,0x85};
constexpr std::uint8_t kNamedValueSetGetSig[] = {0x48,0x89,0x5C,0x24,0x10,0x48,0x89,0x74,0x24,0x18,0x48,0x89,0x7C,0x24,0x20,0x41};
constexpr std::uint8_t kVariantCastToIntSig[] = {0x48,0x83,0xEC,0x38,0x48,0x63,0x41,0x18,0x4C,0x8B,0xC1,0x83,0xF8,0x09};
constexpr std::uint8_t kCombatRunForcedLoadSig[] = {0x0F,0xB7,0x05,0x02,0x7C,0x50,0x02};
constexpr std::uint8_t kCombatRunApplyLoadSig[] = {0x0F,0xB7,0x05,0x1F,0x7B,0x50,0x02};
constexpr std::uint8_t kCombatStatusLoadSig[] = {0x0F,0xB7,0x05,0xC1,0x7A,0x50,0x02};
constexpr std::size_t kNamedValueSetOffset = 0x2A8;  // NamedValueComponent -> its NamedValueSet (retail)
using NamedValueSetIntFn = NamedValueRef*(__fastcall*)(NamedValueRef* self, int value);
using NamedValueComponentFn = std::uint8_t*(__fastcall*)(NamedValueRef* self);
using NamedValueSetGetFn = void*(__fastcall*)(void* set, const std::uint16_t* symbol, bool create);
using VariantCastToIntFn = int(__fastcall*)(const void* variant);
NamedValueSetIntFn g_namedValueSetInt = nullptr;
NamedValueComponentFn g_namedValueComponent = nullptr;
NamedValueSetGetFn g_namedValueSetGet = nullptr;
VariantCastToIntFn g_variantCastToInt = nullptr;
const std::uint16_t* g_symCombatRunForced = nullptr;
const std::uint16_t* g_symCombatRunApply = nullptr;
const std::uint16_t* g_symCombatStatus = nullptr;
std::atomic_bool g_combatRunAvailable{false};
using NamedValueCtorFn = NamedValueRef*(__fastcall*)(NamedValueRef* self, void* instance, const std::uint16_t* symbol);
using NamedValueSetBoolFn = NamedValueRef*(__fastcall*)(NamedValueRef* self, bool value);
using SymbolCStrFn = const char*(__fastcall*)(const std::uint16_t* symbol);
NamedValueCtorFn g_namedValueCtor = nullptr;
NamedValueSetBoolFn g_namedValueSetBool = nullptr;
SymbolCStrFn g_omniSymbolCStr = nullptr;
const std::uint16_t* g_symForceOn = nullptr;  // gNamedValue_PlayerMovement_Omnidirectional_ForceOn (set at game start)
const std::uint16_t* g_symAllows = nullptr;   // gNamedValue_PlayerMovement_Omnidirectional_AnimGraphAllows
std::atomic_bool g_omniValuesAvailable{false};
struct OmniValues {  // game thread only
    void* instance = nullptr;  // where they are on
    bool on = false;
    ULONGLONG lastMs = 0;
    bool loggedNames = false;
    std::uint32_t logs = 0;
};
OmniValues g_omniValues;
std::atomic<std::uint32_t> g_omniValueWrites{0};

void RemapPadButtons(XINPUT_STATE* state) {
    bool strafe = false;
    if (state) {
        g_padStick.store(static_cast<std::uint16_t>(state->Gamepad.sThumbLX) |
                             (static_cast<std::uint32_t>(static_cast<std::uint16_t>(state->Gamepad.sThumbLY)) << 16),
                         std::memory_order_relaxed);
        g_padButtons.store(state->Gamepad.wButtons, std::memory_order_relaxed);
        g_padLeftTrigger.store(state->Gamepad.bLeftTrigger, std::memory_order_relaxed);   // (v0.9.582)
        g_padReadMs.store(GetTickCount64(), std::memory_order_relaxed);
    }
    if (state && g_lbStrafe.load(std::memory_order_relaxed)) {
        WORD& buttons = state->Gamepad.wButtons;
        const bool lb = (buttons & XINPUT_GAMEPAD_LEFT_SHOULDER) != 0, rb = (buttons & XINPUT_GAMEPAD_RIGHT_SHOULDER) != 0;
        const bool aiming = state->Gamepad.bLeftTrigger > XINPUT_GAMEPAD_TRIGGER_THRESHOLD;
        if (lb && !aiming) {
            if (rb) {
                buttons = static_cast<WORD>(buttons & ~XINPUT_GAMEPAD_RIGHT_SHOULDER);  // LB+RB: the game's LB (heal panel)
            } else {
                buttons = static_cast<WORD>(buttons & ~XINPUT_GAMEPAD_LEFT_SHOULDER);   // LB alone: our strafe
                strafe = true;
            }
        }
    }
    const bool was = g_strafeHeld.exchange(strafe, std::memory_order_relaxed);
    if (strafe && !was) g_strafePresses.fetch_add(1, std::memory_order_relaxed);
    if (state) Brawl::RemapPadButtons(state->Gamepad.wButtons);  // X next to an enemy: the game's melee button
}

DWORD WINAPI HookXInput13(DWORD user, XINPUT_STATE* state) {
    const DWORD result = g_origXInput13(user, state);
    if (result == ERROR_SUCCESS) CutsceneViewer::FilterPad(user, state);   // the viewer's: emptied for the game
    if (result == ERROR_SUCCESS) RemapPadButtons(state);
    return result;
}

DWORD WINAPI HookXInput910(DWORD user, XINPUT_STATE* state) {
    const DWORD result = g_origXInput910(user, state);
    if (result == ERROR_SUCCESS) CutsceneViewer::FilterPad(user, state);
    if (result == ERROR_SUCCESS) RemapPadButtons(state);
    return result;
}

// Status_Move_Omnidirectional / _Allowed for Lara while the strafe is held; the NOT bit and the compared value as the
// game's own.
bool OmniAnswer(int k, void* agent, void* memory, const std::uint8_t* condition) {
    if (g_strafeHeld.load(std::memory_order_relaxed) && agent == g_omniAgent.load(std::memory_order_relaxed) && condition) {
        bool result = *reinterpret_cast<const std::int32_t*>(condition + kConditionValue) != 0;  // status true == value?
        if (condition[kConditionFlags] & 1) result = !result;
        g_omniAnswers.fetch_add(1, std::memory_order_relaxed);
        return result;
    }
    const ConditionFn original = g_omniOverrides[k].original;
    return original ? original(agent, memory, condition) : false;
}

bool __fastcall OmniCondition(void* agent, void* memory, const std::uint8_t* condition) {
    return OmniAnswer(0, agent, memory, condition);
}

bool __fastcall OmniAllowedCondition(void* agent, void* memory, const std::uint8_t* condition) {
    return OmniAnswer(1, agent, memory, condition);
}

ConditionFn* OmniConditionSlot(void* agent, std::uint16_t condition) {
    return reinterpret_cast<ConditionFn*>(static_cast<std::uint8_t*>(agent) + kAgentConditionTable +
                                          static_cast<std::size_t>(condition) * kConditionEntrySize + kConditionEntryFunction);
}

// Game thread: our functions in the primary agent's table entries for conds 247 / 248 (again for a new agent), or the
// game's.
void UpdateOmniConditionGameThread(void* agent, bool want) {
    void* current = g_omniAgent.load();
    if (current && (!want || current != agent)) {
        // the old agent: put the game's functions back unless the agent (its memory) changed under us
        if (current == agent) {
#if defined(_MSC_VER)
            __try {
#endif
                for (const OmniOverride& o : g_omniOverrides) {
                    ConditionFn* slot = OmniConditionSlot(current, o.condition);
                    if (o.original && *slot == o.ours) *slot = o.original;
                }
#if defined(_MSC_VER)
            } __except (EXCEPTION_EXECUTE_HANDLER) {
            }
#endif
        }
        g_omniAgent.store(nullptr);
    }
    if (!want || !agent || g_omniAgent.load() == agent) return;
#if defined(_MSC_VER)
    __try {
#endif
        ConditionFn found[2] = {};
        for (int k = 0; k < 2; ++k) {
            const OmniOverride& o = g_omniOverrides[k];
            found[k] = *OmniConditionSlot(agent, o.condition);
            if (!found[k]) return;
            if (found[k] != o.ours && o.original && found[k] != o.original) {
                static int logged = 0;
                if (logged++ < 3)
                    Log::Writef("Acro: LB strafe: the condition function of %u changed (%p, was %p); left alone", o.condition,
                                found[k], o.original);
                return;
            }
        }
        for (int k = 0; k < 2; ++k) {
            OmniOverride& o = g_omniOverrides[k];
            if (found[k] == o.ours) continue;
            o.original = found[k];
            *OmniConditionSlot(agent, o.condition) = o.ours;
        }
        g_omniAgent.store(agent);
        Log::DetailOncef("Acro: LB strafe ready (agent %p, Status_Move_Omnidirectional and _Allowed -> ours while LB is held; pad "
                    "hooks %s; omni animation values %s)", agent,
                    g_padHooks.load() ? "on" : "OFF - no xinput1_3 / xinput9_1_0 found",
                    g_omniValuesAvailable.load() ? "ready" : "unavailable");
#if defined(_MSC_VER)
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
#endif
}

// ---------- LB strafe: no sprint (v0.9.270) ----------
// The sprint is the Sub-State Modifiers agent's ground_sprint (SC 267390, agent root 0x29910): 'ground_to_sprint' needs
// Status_SprintAllowed (cond 211) and 'sprint_disallowed_to_default' leaves on NOT Status_SprintAllowed. While LB strafe
// is held that agent's table entry answers "not allowed": no sprint starts and a running one ends, so the strafe plays
// its omnidirectional cycles instead of the forward-only sprint (v0.9.268 trace: LB pressed in a sprint kept ground_sprint
// and graph 267234 Move_States in its Sprint pipe).
constexpr std::uint16_t kCondSprintAllowed = 211;  // dtp::EnumSCCondProperty Status_SprintAllowed
constexpr const char* kModifiersAgentName = "Sub-State Modifiers";
std::atomic<void*> g_sprintAgent{nullptr};
std::atomic<ConditionFn> g_sprintOriginal{nullptr};
std::atomic<std::uint32_t> g_sprintAnswers{0};

bool __fastcall SprintAllowedCondition(void* agent, void* memory, const std::uint8_t* condition) {
    if (g_strafeHeld.load(std::memory_order_relaxed) && agent == g_sprintAgent.load(std::memory_order_relaxed) && condition) {
        bool result = *reinterpret_cast<const std::int32_t*>(condition + kConditionValue) == 0;  // status false == value?
        if (condition[kConditionFlags] & 1) result = !result;
        g_sprintAnswers.fetch_add(1, std::memory_order_relaxed);
        return result;
    }
    const ConditionFn original = g_sprintOriginal.load(std::memory_order_relaxed);
    return original ? original(agent, memory, condition) : false;
}

// Lara's state control agent with this debug name, or null.
void* FindLaraAgent(const char* name) {
    void* primary = CombatAir::LaraPrimaryAgent();
    if (!primary) return nullptr;
#if defined(_MSC_VER)
    __try {
#endif
        auto* component = *reinterpret_cast<const std::uint8_t* const*>(static_cast<std::uint8_t*>(primary) + kAgentComponent);
        auto* dtp = component ? *reinterpret_cast<const std::uint8_t* const*>(component + kComponentDtp) : nullptr;
        auto* const* agents = component ? *reinterpret_cast<std::uint8_t* const* const*>(component + kComponentAgents) : nullptr;
        const std::uint32_t count = dtp ? std::min(*reinterpret_cast<const std::uint32_t*>(dtp + 8), 8u) : 0u;
        for (std::uint32_t i = 0; agents && i < count; ++i) {
            std::uint8_t* agent = agents[i];
            auto* data = agent ? *reinterpret_cast<const std::uint8_t* const*>(agent + kAgentData) : nullptr;
            const char* agentName = data ? *reinterpret_cast<const char* const*>(data + kAgentDataName) : nullptr;
            if (agentName && std::strcmp(agentName, name) == 0) return agent;
        }
#if defined(_MSC_VER)
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
#endif
    return nullptr;
}

// Game thread (tick): our function in the Sub-State Modifiers agent's entry for cond 211 while the LB strafe is on (again
// for a new agent), the game's otherwise.
void UpdateSprintConditionGameThread(bool want) {
    void* agent = FindLaraAgent(kModifiersAgentName);
    void* current = g_sprintAgent.load();
    if (current && (!want || current != agent)) {
        if (current == agent) {  // still Lara's: put the game's function back
#if defined(_MSC_VER)
            __try {
#endif
                ConditionFn* slot = OmniConditionSlot(current, kCondSprintAllowed);
                const ConditionFn original = g_sprintOriginal.load();
                if (original && *slot == &SprintAllowedCondition) *slot = original;
#if defined(_MSC_VER)
            } __except (EXCEPTION_EXECUTE_HANDLER) {
            }
#endif
        }
        g_sprintAgent.store(nullptr);
    }
    if (!want || !agent || g_sprintAgent.load() == agent) return;
    static std::uint32_t s_logs = 0;
#if defined(_MSC_VER)
    __try {
#endif
        ConditionFn* slot = OmniConditionSlot(agent, kCondSprintAllowed);
        const ConditionFn found = *slot;
        if (!found) return;
        if (found != &SprintAllowedCondition) {
            const ConditionFn known = g_sprintOriginal.load();
            if (known && found != known) {
                if (s_logs++ < 3)
                    Log::Writef("Acro: LB strafe: the Status_SprintAllowed function changed (%p, was %p); left alone", found, known);
                return;
            }
            g_sprintOriginal.store(found);
            *slot = &SprintAllowedCondition;
        }
        g_sprintAgent.store(agent);
        if (s_logs++ < 6)
            Log::DetailOncef("Acro: LB strafe: no sprint while LB is held (Sub-State Modifiers agent %p, Status_SprintAllowed -> "
                        "ours)", agent);
#if defined(_MSC_VER)
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
#endif
}

bool SetNamedBool(void* instance, std::uint16_t symbol, bool value) {
    if (!instance || !symbol || !g_namedValueCtor || !g_namedValueSetBool) return false;
    NamedValueRef named{};
#if defined(_MSC_VER)
    __try {
#endif
        g_namedValueCtor(&named, instance, &symbol);
        g_namedValueSetBool(&named, value);
#if defined(_MSC_VER)
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
#endif
    return true;
}

const char* OmniSymbolName(std::uint16_t symbol) {
    if (!symbol || !g_omniSymbolCStr) return "?";
    const char* name = nullptr;
#if defined(_MSC_VER)
    __try {
#endif
        name = g_omniSymbolCStr(&symbol);
#if defined(_MSC_VER)
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        name = nullptr;
    }
#endif
    return name ? name : "?";
}

// The game's own Status_Move_Omnidirectional / _Allowed for the agent now (a made-up "== true" condition), for the log.
int NativeOmniAnswer(void* agent, int k) {
    const ConditionFn original = g_omniOverrides[k].original;
    if (!agent || !original) return -1;
    alignas(8) std::uint8_t condition[0x28] = {};
    *reinterpret_cast<std::int32_t*>(condition + kConditionValue) = 1;
    int result = -1;
#if defined(_MSC_VER)
    __try {
#endif
        result = original(agent, nullptr, condition) ? 1 : 0;
#if defined(_MSC_VER)
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        result = -1;
    }
#endif
    return result;
}

// ---------- LB strafe: no cover crouch ----------
// Near cover (and in bushes) PSCAF_P_Combat_ProcessCover calls these every frame with what it detects; they set the
// named values cover_crouch / cover_standing_exploration / cover_standing, which make graph 267234 Move_States / Run and
// Walk play their "Cover" pipe (the crouch run, forward only). While LB strafe is held Lara's get false: she stands and
// strafes on the omnidirectional cycles; the frame after LB is let go they get the game's values again. (Under a low
// ceiling the graph still crouches.) CoverState +0 = its Instance.
constexpr std::uintptr_t kRvaSetLowCover = 0x0084EE60ull;       // CoverState::SetLowCover(bool)
constexpr std::uintptr_t kRvaSetStandingCover = 0x0084FF80ull;  // CoverState::SetStandingCover(bool)
constexpr std::uintptr_t kRvaSetCornerLean = 0x0084DEE0ull;     // CoverState::SetCornerLean(bool)
constexpr std::uint8_t kSetLowCoverSig[] = {
    0x4C,0x8B,0xDC,0x53,0x56,0x41,0x56,0x48,0x83,0xEC,0x50,0x48,0x8B,0x19,0x0F,0xB6,0xF2,0x4C,0x8B,0xF1,0x48,0x85,0xDB,0x0F
};
constexpr std::uint8_t kSetStandingCoverSig[] = {
    0x48,0x89,0x5C,0x24,0x10,0x48,0x89,0x74,0x24,0x18,0x57,0x48,0x83,0xEC,0x30,0x48,0x8B,0x31,0x0F,0xB6,0xFA,0x48,0x8B,0xD9
};
constexpr std::uint8_t kSetCornerLeanSig[] = {
    0x48,0x89,0x5C,0x24,0x18,0x48,0x89,0x74,0x24,0x20,0x57,0x48,0x83,0xEC,0x40,0x48,0x8B,0x19,0x0F,0xB6,0xF2,0x48,0x8B,0xF9
};
using CoverSetterFn = void(__fastcall*)(void* cover, bool value);
CoverSetterFn g_origSetLowCover = nullptr;
CoverSetterFn g_origSetStandingCover = nullptr;
CoverSetterFn g_origSetCornerLean = nullptr;
std::atomic<int> g_coverHooks{0};
std::atomic<std::uint32_t> g_coverKeptOff{0};
std::atomic<std::uint32_t> g_coverLogs{0};

void* LaraInstanceNow() {
    void* agent = CombatAir::LaraPrimaryAgent();
    void* instance = nullptr;
#if defined(_MSC_VER)
    __try {
#endif
        instance = agent ? *reinterpret_cast<void**>(static_cast<std::uint8_t*>(agent) + kAgentInstance) : nullptr;
#if defined(_MSC_VER)
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        instance = nullptr;
    }
#endif
    return instance;
}

// True when a cover setter is asked to turn Lara's cover on while the LB strafe is held.
bool StrafeKeepsCoverOff(void* cover, bool value, const char* which) {
    if (!value || !cover || !g_lbStrafe.load(std::memory_order_relaxed) || !g_padHooks.load(std::memory_order_relaxed) ||
        !g_strafeHeld.load(std::memory_order_relaxed))
        return false;
    void* owner = nullptr;
#if defined(_MSC_VER)
    __try {
#endif
        owner = *static_cast<void* const*>(cover);
#if defined(_MSC_VER)
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
#endif
    if (!owner || owner != LaraInstanceNow()) return false;
    g_coverKeptOff.fetch_add(1, std::memory_order_relaxed);
    if (g_coverLogs.fetch_add(1, std::memory_order_relaxed) < 8)
        Log::Detailf("Acro: LB strafe: Lara's cover kept off (%s; she stands to strafe)", which);
    return true;
}

// When the game last wanted Lara in low cover (SetLowCover(true) for her), whether or not the strafe kept it off.
std::atomic<ULONGLONG> g_lowCoverWantedMs{0};
std::atomic<void*> g_laraCover{nullptr};  // Lara's CoverState (the setters' this), for turning her cover off at the strafe

// True for Lara's own CoverState (+0 = its Instance), which is remembered.
bool IsLaraCover(void* cover) {
    if (!cover) return false;
    void* owner = nullptr;
#if defined(_MSC_VER)
    __try {
#endif
        owner = *static_cast<void* const*>(cover);
#if defined(_MSC_VER)
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
#endif
    if (!owner || owner != LaraInstanceNow()) return false;
    g_laraCover.store(cover, std::memory_order_relaxed);
    return true;
}

void __fastcall HookSetLowCover(void* cover, bool value) {
    if (IsLaraCover(cover) && value && g_strafeCrouch.load(std::memory_order_relaxed))
        g_lowCoverWantedMs.store(GetTickCount64(), std::memory_order_relaxed);
    if (StrafeKeepsCoverOff(cover, value, "low cover / bushes")) value = false;
    g_origSetLowCover(cover, value);
}

void __fastcall HookSetStandingCover(void* cover, bool value) {
    IsLaraCover(cover);
    if (StrafeKeepsCoverOff(cover, value, "standing cover")) value = false;
    g_origSetStandingCover(cover, value);
}

void __fastcall HookSetCornerLean(void* cover, bool value) {
    IsLaraCover(cover);
    if (StrafeKeepsCoverOff(cover, value, "corner lean")) value = false;
    g_origSetCornerLean(cover, value);
}

// Game thread, every frame: at the LB press (and every 0.5 s while it is held) Lara's cover goes off through the game's
// own setters. The game's cover check does not run in the omnidirectional states, so a cover she was in when LB went down
// stayed on and the graph kept playing its forward-only crouch run (v0.9.268 trace: cover_crouch_run / _walk under
// ground_omnidirectional_run, and not one SetLowCover call to keep off).
void ClearCoverForStrafeGameThread() {
    static bool s_held = false;
    static ULONGLONG s_next = 0;
    static std::uint32_t s_logs = 0;
    const bool held = g_lbStrafe.load(std::memory_order_relaxed) && g_padHooks.load(std::memory_order_relaxed) &&
                      g_strafeHeld.load(std::memory_order_relaxed);
    const ULONGLONG now = GetTickCount64();
    if (!held) {
        s_held = false;
        return;
    }
    const bool press = !s_held;
    if (!press && now < s_next) return;
    s_held = true;
    s_next = now + 500ull;
    void* cover = g_laraCover.load(std::memory_order_relaxed);
    if (!cover || !g_origSetLowCover || !g_origSetStandingCover || !g_origSetCornerLean || !IsLaraCover(cover)) return;
    g_origSetCornerLean(cover, false);
    g_origSetStandingCover(cover, false);
    g_origSetLowCover(cover, false);
    if (press && s_logs++ < 8) Log::Detailf("Acro: LB strafe: Lara's cover turned off at the press (CoverState %p)", cover);
}

// ---------- crouched strafe: the omnidirectional pipes' entries ----------
void SetStrafeCrouchEntries(bool on) {
    AnimEntry* entries = g_patchedEntries.load();
    const AnimEntry* original = g_originalEntries;
    if (!entries || !original) {
        g_strafeCrouchActive.store(false);
        return;
    }
    int written = 0;
    for (std::size_t i = 0; i < g_defCount; ++i) {
        const int e = g_defs[i].strafeCrouch ? g_climbEntry[i] : -1;
        if (e < 0 || static_cast<std::uint32_t>(e) >= g_originalCount || g_clips[i].id < 0) continue;
        if (on) {
            const auto id = static_cast<std::int16_t>(g_clips[i].id);
            WriteEntry(&entries[e], id, id, original[e].fx);
        } else {
            WriteEntry(&entries[e], original[e].animId, original[e].animNum, original[e].fx);
        }
        ++written;
    }
    g_strafeCrouchActive.store(on && written > 0);
    static int s_logs = 0;
    if (written && s_logs++ < 20)
        Log::Detailf("Acro: crouched strafe %s (%d entries)", on ? "on: near low cover" : "off", written);
}

// Game thread, every frame (before the named values): the crouched cycles while LB strafe is held and the game wants Lara
// in low cover (asked within 0.4 s); the game's cycles otherwise. Once on they stay for the rest of the hold: the game
// stops asking a moment after the strafe starts (v0.9.266 log: on, then off within a second, the entries swapping
// back and forth under a running strafe).
void UpdateStrafeCrouchGameThread() {
    const bool active = g_strafeCrouchActive.load(std::memory_order_relaxed);
    const bool strafing = g_lbStrafe.load(std::memory_order_relaxed) && g_padHooks.load(std::memory_order_relaxed) &&
                          g_strafeHeld.load(std::memory_order_relaxed);
    bool want = false;
    if (strafing && g_strafeCrouch.load(std::memory_order_relaxed) && g_patched) {
        const ULONGLONG wanted = g_lowCoverWantedMs.load(std::memory_order_relaxed);
        want = active || (wanted && GetTickCount64() - wanted < 400ull);
    }
    if (want != active) SetStrafeCrouchEntries(want);
}

// ---------- LB strafe trace (v0.9.268) ----------
// The v0.9.266 test: the strafe froze on one frame near low cover and in a sprint. While the LB strafe is held (and 1.5 s
// after) one line every 0.4 s: Lara's speed over the ground, the left stick and L3, every state control agent's state,
// the crouched cycles and the last low-cover wish, and the fragment nodes her graph updated last: library entry (our clip
// in brackets), blend weight, frame / frames and how far the node's time moved in its last update ("still": a clip whose
// time stands still, a frozen pose; "done": a non-looping node at its end). Her agents' state changes are logged
// meanwhile (OnStateTransition). [Acrobatics] StrafeTrace=0 turns it off.
constexpr std::uintptr_t kRvaFragmentUpdate = 0x001381B0ull;  // cdc::AnimFragmentNode::Update (vftable 0xD14F48 [7])
constexpr std::uint8_t kFragmentUpdateSig[] = {
    0x4C,0x8B,0xDC,0x55,0x53,0x56,0x41,0x56,0x41,0x57,0x49,0x8D,0x6B,0xA1,0x48,0x81,
    0xEC,0xD0,0x00,0x00,0x00,0x48,0x8B,0x02,0x4D,0x8B,0xF0,0x4C,0x63,0x49,0x38
};
// cdc::AnimFragmentNode: +0x10 m_owner (AnimGraphRoot, u32 library key +0), +0x38 mOpDataID, +0x40 dtp::FragmentNode
// (u16 Anim = library entry), +0x48 mKeyList (AnimFragment: u16 mKeyCount +0x32, u16 mTimePerKey +0x34).
// cdc::AnimOperationalData: +0 node op data table, +0x18 Instance. Node op data: +0x60 mElapsedTime, +0x64 traversal of
// its last update (Update returns at once when it matches), +0x68 flags (bit 0 done). cdc::AnimUpdateData: +0 dt,
// +0x14 weight, +0x18 traversalId.
constexpr std::size_t kNodeOwner = 0x10, kNodeOpDataId = 0x38, kNodeDtp = 0x40, kNodeKeyList = 0x48;
constexpr std::size_t kAnimDataInstance = 0x18;
constexpr std::size_t kOpElapsed = 0x60, kOpTraversal = 0x64, kOpFlags = 0x68;
constexpr std::size_t kUpdateDt = 0x00, kUpdateWeight = 0x14, kUpdateTraversal = 0x18;
constexpr std::size_t kKeyListKeyCount = 0x32, kKeyListTimePerKey = 0x34;
constexpr std::uint32_t kTraceLineLimit = 400, kTraceTransitionLimit = 300;
using FragmentUpdateFn = void(__fastcall*)(void* node, void* animData, const std::uint8_t* update);
FragmentUpdateFn g_origFragmentUpdate = nullptr;
std::atomic_bool g_fragmentHook{false};
std::atomic_bool g_traceOn{false};
std::atomic<void*> g_traceInstance{nullptr};
std::atomic<std::uint32_t> g_traceTransitions{0};
struct TraceNode {
    const void* node;
    const std::uint8_t* keyList;
    ULONGLONG ms;
    std::uint32_t library;
    std::uint16_t entry;
    std::uint8_t flags;
    float weight, dt, before, after;
};
constexpr int kTraceNodes = 48;
TraceNode g_traceNodes[kTraceNodes] = {};
SRWLOCK g_traceLock = SRWLOCK_INIT;

// Lara's node op data for this update, or null (another instance's graph, or a node already updated this traversal).
const std::uint8_t* TraceOpData(void* node, void* animData, const std::uint8_t* update, void* lara, float* before) {
#if defined(_MSC_VER)
    __try {
#endif
        if (*reinterpret_cast<void* const*>(static_cast<std::uint8_t*>(animData) + kAnimDataInstance) != lara) return nullptr;
        auto* const* table = *reinterpret_cast<std::uint8_t* const* const*>(animData);
        const std::int32_t id = *reinterpret_cast<const std::int32_t*>(static_cast<std::uint8_t*>(node) + kNodeOpDataId);
        const std::uint8_t* op = table && id >= 0 ? table[id] : nullptr;
        if (!op || *reinterpret_cast<const std::uint32_t*>(op + kOpTraversal) ==
                       *reinterpret_cast<const std::uint32_t*>(update + kUpdateTraversal))
            return nullptr;
        *before = *reinterpret_cast<const float*>(op + kOpElapsed);
        return op;
#if defined(_MSC_VER)
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
#endif
}

void RecordTraceNode(void* node, const std::uint8_t* op, const std::uint8_t* update, float before) {
    TraceNode t{};
    t.node = node;
    t.before = before;
#if defined(_MSC_VER)
    __try {
#endif
        const auto* n = static_cast<const std::uint8_t*>(node);
        const auto* owner = *reinterpret_cast<const std::uint8_t* const*>(n + kNodeOwner);
        t.library = owner ? *reinterpret_cast<const std::uint32_t*>(owner) : 0;
        const auto* dtp = *reinterpret_cast<const std::uint8_t* const*>(n + kNodeDtp);
        t.entry = dtp ? *reinterpret_cast<const std::uint16_t*>(dtp) : 0xFFFF;
        t.keyList = *reinterpret_cast<const std::uint8_t* const*>(n + kNodeKeyList);
        t.flags = op[kOpFlags];
        t.after = *reinterpret_cast<const float*>(op + kOpElapsed);
        t.dt = *reinterpret_cast<const float*>(update + kUpdateDt);
        t.weight = *reinterpret_cast<const float*>(update + kUpdateWeight);
#if defined(_MSC_VER)
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return;
    }
#endif
    t.ms = GetTickCount64();
    AcquireSRWLockExclusive(&g_traceLock);
    int slot = -1;
    for (int i = 0; i < kTraceNodes && slot < 0; ++i)
        if (g_traceNodes[i].node == node) slot = i;
    if (slot < 0) {
        slot = 0;
        for (int i = 1; i < kTraceNodes; ++i)
            if (g_traceNodes[i].ms < g_traceNodes[slot].ms) slot = i;
    }
    g_traceNodes[slot] = t;
    ReleaseSRWLockExclusive(&g_traceLock);
}

// ---------- LB strafe in combat: the in-combat omnidirectional cycles (v0.9.270) ----------
// Graph 267234 (Lara's ground graph) has four Move_Oriented_States: mood_default Run / Walk with the oriented and
// omnidirectional cycles, and mood_InCombat Run / Walk (while Combat_IsPlayerCombatStatus, cond 584, is 1 or 2) whose
// fragments all have Anim = NONE. Since v0.9.264 turned the combat run off for the strafe, the strafe in a combat
// encounter played no clip at all and nothing of Lara's graph updated any more (v0.9.268 trace: "none updated" in
// ground_omnidirectional_run; in game: stuck on one frame). The in-combat fragments get the default ones' entries
// (dtp::FragmentNode +0 u16 Anim, +2 loop mode, +4 play speed; AnimFragmentNode::Activate reads Anim at every
// activation). Offsets are graph 267234 body offsets (re/anim_graph/267234.tr10dtp.graph.txt, paired by position);
// both sides are checked before anything is written. The graph is found from the fragments Lara's graph updates (the
// hook below): a node of a known fragment gives the body's address. Those pipes are only reached with the
// omnidirectional movement on (our strafe), so the fill stays.
struct FragmentFill {
    std::uint32_t combat;  // the in-combat fragment's dtp::FragmentNode
    std::uint32_t source;  // the mood_default fragment it copies
    std::uint16_t anim;    // the library entry both end up with
};
constexpr FragmentFill kCombatOmniFill[] = {
    // Run: Oriented States Front, Left, Right, Back, Back_Flipped, Left Back, Right Back
    {0x78958, 0x551e8, 235}, {0x78ad8, 0x55368, 231}, {0x78b98, 0x55428, 234}, {0x78c58, 0x554e8, 233},
    {0x78a18, 0x552a8, 232}, {0x78d18, 0x555a8, 230}, {0x78dd8, 0x55668, 229},
    // Run: Omnidirectional Forward, Back, Left (forward / backward leg), Right (forward / backward leg), Idle, stairs
    {0x76c10, 0x53288, 235}, {0x76ff8, 0x536b8, 674}, {0x776a8, 0x53d68, 666}, {0x775d8, 0x53c98, 668},
    {0x77d88, 0x54448, 665}, {0x77cb8, 0x54378, 667}, {0x77de0, 0x544a0, 2},   {0x78848, 0x550d8, 237},
    {0x78820, 0x550b0, 236},
    // Walk: Oriented States
    {0x813e0, 0x5df58, 174}, {0x81560, 0x5e0d8, 173}, {0x81620, 0x5e198, 172}, {0x816e0, 0x5e258, 171},
    {0x814a0, 0x5e018, 170}, {0x817a0, 0x5e318, 168}, {0x81860, 0x5e3d8, 169},
    // Walk: Omnidirectional
    {0x7f698, 0x5c210, 174}, {0x7fa80, 0x5c5f8, 673}, {0x80130, 0x5cca8, 671}, {0x80060, 0x5cbd8, 669},
    {0x80810, 0x5d388, 672}, {0x80740, 0x5d2b8, 670}, {0x80868, 0x5d3e0, 2},   {0x812d0, 0x5de48, 174},
    {0x812a8, 0x5de20, 174},
};
// Fragments of graph 267234 that play often (idle, the run / walk / cover / combat / sprint cycles of both moods): a
// node of one of them gives a candidate body address (node props - offset), which the table above must confirm.
struct KnownFragment {
    std::uint32_t props;
    std::uint16_t anim;
};
constexpr KnownFragment kGraph267234Known[] = {
    {0xAFCD8, 2},    {0xD35D0, 2},    {0x50370, 235},  {0x50398, 419},  {0x73DD8, 235},  {0x73E00, 419},
    {0x55DF8, 1029}, {0x55E20, 1029}, {0x79548, 1029}, {0x79570, 1029}, {0x569D8, 1021}, {0x56A00, 1020},
    {0x7A018, 1021}, {0x7A040, 1020}, {0x5EB48, 1019}, {0x5EB70, 1019}, {0x81FD0, 1019}, {0x81FF8, 1019},
    {0x593D8, 174},  {0x59400, 174},  {0x600F8, 266},  {0x60120, 266},  {0x835C8, 266},  {0x835F0, 266},
};
std::atomic<std::uint8_t*> g_combatOmniBase{nullptr};  // graph 267234's body once filled
std::atomic_bool g_combatOmniWanted{true};              // the hook collects candidates until then
constexpr int kFillCandidates = 32;
std::atomic<std::uint64_t> g_fillCandidates[kFillCandidates] = {};  // props address | entry << 48, from Lara's nodes
std::atomic<std::uint32_t> g_fillCandidateNext{0};

bool IsKnownFragmentAnim(std::uint16_t anim) {
    for (const KnownFragment& k : kGraph267234Known)
        if (k.anim == anim) return true;
    return false;
}

// In the fragment hook while wanted: Lara's nodes of the known fragments' entries.
void CollectFillCandidate(void* node, void* animData) {
    void* lara = g_traceInstance.load(std::memory_order_relaxed);
    if (!lara) return;
#if defined(_MSC_VER)
    __try {
#endif
        if (*reinterpret_cast<void* const*>(static_cast<std::uint8_t*>(animData) + kAnimDataInstance) != lara) return;
        const auto* props = *reinterpret_cast<const std::uint8_t* const*>(static_cast<std::uint8_t*>(node) + kNodeDtp);
        if (!props) return;
        const std::uint16_t entry = *reinterpret_cast<const std::uint16_t*>(props);
        if (!IsKnownFragmentAnim(entry)) return;
        const std::uint32_t slot = g_fillCandidateNext.fetch_add(1, std::memory_order_relaxed) % kFillCandidates;
        g_fillCandidates[slot].store(reinterpret_cast<std::uintptr_t>(props) | (static_cast<std::uint64_t>(entry) << 48),
                                     std::memory_order_relaxed);
#if defined(_MSC_VER)
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
#endif
}

// True when [from, to) is committed, readable memory without guard pages (a candidate address is only a guess).
bool ReadableRange(const void* from, const void* to) {
    // (a guarded probe: VirtualQuery of the heap region cost ~7 ms every 2 s in the v0.9.334 log)
    const auto* begin = static_cast<const std::uint8_t*>(from);
    const auto* end = static_cast<const std::uint8_t*>(to);
    return end > begin && MemoryProbe::Readable(begin, static_cast<std::size_t>(end - begin));
}

// 1 = the body at base matches (both sides as expected, the in-combat side NONE or already ours), 0 = not graph 267234
// (or not this layout), -1 = unreadable. With write: fills the in-combat side.
int CombatOmniFillUnsafe(std::uint8_t* base, bool write, int* written) {
    if (!ReadableRange(base + 0x50000, base + 0x82000)) return -1;  // every offset of the table lies in there
#if defined(_MSC_VER)
    __try {
#endif
        for (const FragmentFill& f : kCombatOmniFill) {
            const std::uint8_t* source = base + f.source;
            const std::uint8_t* combat = base + f.combat;
            const std::uint16_t now = *reinterpret_cast<const std::uint16_t*>(combat);
            if (*reinterpret_cast<const std::uint16_t*>(source) != f.anim || source[2] != 1 || combat[2] != 1 ||
                (now != 0xFFFF && now != f.anim) || *reinterpret_cast<const float*>(source + 4) != 1.0f ||
                *reinterpret_cast<const float*>(combat + 4) != 1.0f)
                return 0;
        }
        if (!write) return 1;
        for (const FragmentFill& f : kCombatOmniFill) {
            auto* anim = reinterpret_cast<std::uint16_t*>(base + f.combat);
            if (*anim == f.anim) continue;
            if (!MakeWritable(anim, 2)) return 0;
            *anim = f.anim;
            ++*written;
        }
        return 1;
#if defined(_MSC_VER)
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
#endif
}

// True while the in-combat side still has our entries (and the default side its own).
bool CombatOmniFilledUnsafe(std::uint8_t* base) {
    int written = 0;
    if (CombatOmniFillUnsafe(base, false, &written) != 1) return false;
#if defined(_MSC_VER)
    __try {
#endif
        for (const FragmentFill& f : kCombatOmniFill)
            if (*reinterpret_cast<const std::uint16_t*>(base + f.combat) != f.anim) return false;
#if defined(_MSC_VER)
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
#endif
    return true;
}

// Game thread: finds graph 267234 from the collected candidates and fills the in-combat cycles; checks every 2 s that
// they are still there (a level load can bring the graph back in new memory).
void UpdateCombatOmniFillGameThread() {
    static ULONGLONG s_nextCheck = 0;
    static std::uint32_t s_logs = 0;
    const ULONGLONG now = GetTickCount64();
    if (std::uint8_t* base = g_combatOmniBase.load(std::memory_order_relaxed)) {
        if (now < s_nextCheck) return;
        s_nextCheck = now + 2000ull;
        if (CombatOmniFilledUnsafe(base)) return;
        g_combatOmniBase.store(nullptr);
        for (auto& c : g_fillCandidates) c.store(0, std::memory_order_relaxed);
        g_combatOmniWanted.store(true);
        if (s_logs++ < 10) Log::Detail("Acro: Lara's ground graph (267234) changed; the in-combat strafe cycles are filled again");
        return;
    }
    if (!g_fragmentHook.load(std::memory_order_relaxed) || !g_lbStrafe.load(std::memory_order_relaxed)) return;
    for (auto& c : g_fillCandidates) {
        const std::uint64_t value = c.exchange(0, std::memory_order_relaxed);
        if (!value) continue;
        const auto* props = reinterpret_cast<std::uint8_t*>(static_cast<std::uintptr_t>(value & 0xFFFFFFFFFFFFull));
        const auto entry = static_cast<std::uint16_t>(value >> 48);
        for (const KnownFragment& k : kGraph267234Known) {
            if (k.anim != entry || reinterpret_cast<std::uintptr_t>(props) < k.props) continue;
            auto* base = const_cast<std::uint8_t*>(props) - k.props;
            int written = 0;
            if (CombatOmniFillUnsafe(base, false, &written) != 1) continue;
            const int result = CombatOmniFillUnsafe(base, true, &written);
            if (result != 1) {
                if (s_logs++ < 10) Log::Writef("Acro: in-combat strafe cycles: graph 267234 found at %p but not writable", base);
                return;
            }
            g_combatOmniBase.store(base);
            g_combatOmniWanted.store(false);
            s_nextCheck = now + 2000ull;
            if (s_logs++ < 10)
                Log::Detailf("Acro: in-combat strafe cycles filled: graph 267234 at %p, %d of %zu fragments written (the rest "
                            "already had them)", base, written, sizeof(kCombatOmniFill) / sizeof(kCombatOmniFill[0]));
            return;
        }
    }
}

// Every fragment node of every graph, every frame (any thread the graphs update on): only Lara's are recorded, and only
// while the trace runs; until graph 267234 is found, her nodes of known fragments are collected for it.
std::atomic<Acro::FragmentPreUpdateFn> g_fragmentPreUpdate{nullptr};   // (v0.9.574) Acro::SetFragmentPreUpdate

void __fastcall HookFragmentUpdate(void* node, void* animData, const std::uint8_t* update) {
    if (const Acro::FragmentPreUpdateFn pre = g_fragmentPreUpdate.load(std::memory_order_acquire); pre && node && animData)
        pre(node, animData);   // (v0.9.574) before the trace reads the node's time
    if (g_combatOmniWanted.load(std::memory_order_relaxed) && node && animData) CollectFillCandidate(node, animData);
    void* lara = g_traceOn.load(std::memory_order_relaxed) ? g_traceInstance.load(std::memory_order_relaxed) : nullptr;
    float before = 0.0f;
    const std::uint8_t* op = lara && node && animData && update ? TraceOpData(node, animData, update, lara, &before) : nullptr;
    g_origFragmentUpdate(node, animData, update);
    if (op) RecordTraceNode(node, op, update, before);
}

void CopyStringUnsafe(char* out, std::size_t size, const char* text) {
    out[0] = 0;
#if defined(_MSC_VER)
    __try {
#endif
        if (text) std::snprintf(out, size, "%s", text);
#if defined(_MSC_VER)
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        std::snprintf(out, size, "?");
    }
#endif
}

bool KeyListFrames(const std::uint8_t* keyList, int* keys, int* timePerKey) {
    *keys = *timePerKey = 0;
    if (!keyList) return false;
#if defined(_MSC_VER)
    __try {
#endif
        *keys = *reinterpret_cast<const std::uint16_t*>(keyList + kKeyListKeyCount);
        *timePerKey = *reinterpret_cast<const std::uint16_t*>(keyList + kKeyListTimePerKey);
#if defined(_MSC_VER)
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
#endif
    return *timePerKey > 0;
}

// "Primary Agent=ground_move_run, Secondary Agent=default, ..." for Lara's state control.
int DescribeLaraAgents(char* out, std::size_t size) {
    int length = 0;
    out[0] = 0;
    void* primary = CombatAir::LaraPrimaryAgent();
    if (!primary) return std::snprintf(out, size, "(no agent)");
#if defined(_MSC_VER)
    __try {
#endif
        auto* component = *reinterpret_cast<const std::uint8_t* const*>(static_cast<std::uint8_t*>(primary) + kAgentComponent);
        auto* dtp = component ? *reinterpret_cast<const std::uint8_t* const*>(component + kComponentDtp) : nullptr;
        auto* const* agents = component ? *reinterpret_cast<const std::uint8_t* const* const*>(component + kComponentAgents) : nullptr;
        const std::uint32_t count = dtp ? std::min(*reinterpret_cast<const std::uint32_t*>(dtp + 8), 8u) : 0u;
        for (std::uint32_t i = 0; agents && i < count && length < static_cast<int>(size) - 80; ++i) {
            const std::uint8_t* agent = agents[i];
            if (!agent) continue;
            auto* data = *reinterpret_cast<const std::uint8_t* const*>(agent + kAgentData);
            const char* name = data ? *reinterpret_cast<const char* const*>(data + kAgentDataName) : nullptr;
            auto* state = *reinterpret_cast<const std::uint8_t* const*>(agent + kAgentCurrentState);
            const char* stateName = state ? *reinterpret_cast<const char* const*>(state + kStateName) : nullptr;
            length += std::snprintf(out + length, size - length, "%s%.24s=%.40s", length ? ", " : "", name ? name : "?",
                                    stateName ? stateName : "(none)");
        }
#if defined(_MSC_VER)
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        if (length < static_cast<int>(size) - 16) length += std::snprintf(out + length, size - length, " (read failed)");
    }
#endif
    return length;
}

// The fragment nodes Lara's graph updated in the last 150 ms, heaviest first.
int DescribeTraceNodes(char* out, std::size_t size, ULONGLONG now) {
    TraceNode nodes[kTraceNodes];
    int count = 0;
    AcquireSRWLockShared(&g_traceLock);
    for (const TraceNode& t : g_traceNodes)
        if (t.node && t.ms + 150ull >= now) nodes[count++] = t;
    ReleaseSRWLockShared(&g_traceLock);
    std::sort(nodes, nodes + count, [](const TraceNode& a, const TraceNode& b) { return a.weight > b.weight; });
    int length = 0;
    out[0] = 0;
    for (int i = 0; i < count && length < static_cast<int>(size) - 140; ++i) {
        const TraceNode& t = nodes[i];
        char entry[48], clip[48];
        entry[0] = clip[0] = 0;
        if (t.library == g_laraKey && g_originalEntries && t.entry < g_originalCount)
            CopyStringUnsafe(entry, sizeof(entry), g_originalEntries[t.entry].name);
        if (!entry[0]) std::snprintf(entry, sizeof(entry), "lib %u #%u", t.library, t.entry);
        for (std::size_t c = 0; c < g_defCount; ++c)
            if (t.keyList && g_clips[c].data == t.keyList) {
                std::snprintf(clip, sizeof(clip), "%s", g_defs[c].file);
                if (char* dot = std::strrchr(clip, '.')) *dot = 0;
                break;
            }
        int keys = 0, timePerKey = 0;
        const bool frames = KeyListFrames(t.keyList, &keys, &timePerKey);
        float moved = t.after - t.before;
        if (frames && moved < 0.0f) moved += static_cast<float>(keys * timePerKey);  // looped
        const bool still = t.dt != 0.0f && moved == 0.0f;
        if (frames)
            length += std::snprintf(out + length, size - length, "%s%s%s%s%s w%.2f f%.1f/%d %+.2f%s%s", i ? "; " : "",
                                    entry, clip[0] ? "[" : "", clip, clip[0] ? "]" : "", static_cast<double>(t.weight),
                                    static_cast<double>(t.after / static_cast<float>(timePerKey)), keys,
                                    static_cast<double>(moved / static_cast<float>(timePerKey)), still ? " STILL" : "",
                                    (t.flags & 1) ? " done" : "");
        else
            length += std::snprintf(out + length, size - length, "%s%s (no clip) w%.2f", i ? "; " : "", entry,
                                    static_cast<double>(t.weight));
    }
    if (!count) length = std::snprintf(out, size, "(none updated)");
    return length;
}

bool StrafeHeldNow() {
    return g_lbStrafe.load(std::memory_order_relaxed) && g_padHooks.load(std::memory_order_relaxed) &&
           g_strafeHeld.load(std::memory_order_relaxed);
}

bool ReadGroundPosition(void* instance, float pos[2]) {
#if defined(_MSC_VER)
    __try {
#endif
        pos[0] = *reinterpret_cast<const float*>(static_cast<std::uint8_t*>(instance) + 0x10);  // Instance position (cm)
        pos[1] = *reinterpret_cast<const float*>(static_cast<std::uint8_t*>(instance) + 0x14);
#if defined(_MSC_VER)
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
#endif
    return true;
}

// Game thread, every frame.
void StrafeTraceGameThread() {
    static ULONGLONG s_until = 0, s_next = 0, s_posMs = 0;
    static float s_pos[2] = {};
    static std::uint32_t s_lines = 0;
    const ULONGLONG now = GetTickCount64();
    if (StrafeHeldNow()) s_until = now + 1500ull;
    void* lara = LaraInstanceNow();
    const bool on = g_strafeTrace.load(std::memory_order_relaxed) && g_fragmentHook.load(std::memory_order_relaxed) &&
                    lara && now < s_until && s_lines < kTraceLineLimit;
    g_traceInstance.store(lara, std::memory_order_relaxed);
    g_traceOn.store(on, std::memory_order_relaxed);
    if (!on) {
        s_posMs = 0;
        return;
    }
    if (now < s_next) return;
    s_next = now + 400ull;
    float pos[2] = {};
    if (!ReadGroundPosition(lara, pos)) return;
    char speed[32];
    if (s_posMs && now > s_posMs)
        std::snprintf(speed, sizeof(speed), "%.0f cm/s",
                      static_cast<double>(std::hypot(pos[0] - s_pos[0], pos[1] - s_pos[1]) * 1000.0f /
                                          static_cast<float>(now - s_posMs)));
    else
        std::snprintf(speed, sizeof(speed), "?");
    s_pos[0] = pos[0];
    s_pos[1] = pos[1];
    s_posMs = now;
    const std::uint32_t stick = g_padStick.load(std::memory_order_relaxed);
    const float sx = static_cast<float>(static_cast<std::int16_t>(stick & 0xFFFF)) / 32767.0f;
    const float sy = static_cast<float>(static_cast<std::int16_t>(stick >> 16)) / 32767.0f;
    const bool l3 = (g_padButtons.load(std::memory_order_relaxed) & XINPUT_GAMEPAD_LEFT_THUMB) != 0;
    const ULONGLONG wanted = g_lowCoverWantedMs.load(std::memory_order_relaxed);
    char agents[480], nodes[1400], cover[64], line[2400];
    DescribeLaraAgents(agents, sizeof(agents));
    DescribeTraceNodes(nodes, sizeof(nodes), now);
    if (wanted) std::snprintf(cover, sizeof(cover), "low cover asked %llu ms ago", static_cast<unsigned long long>(now - wanted));
    else std::snprintf(cover, sizeof(cover), "low cover never asked");
    std::snprintf(line, sizeof(line),
                  "Acro: strafe trace: %s, speed %s, stick %.2f at %.0f deg%s, crouched cycles %s (%s, cover kept off %ux); "
                  "agents: %s; nodes: %s",
                  StrafeHeldNow() ? "LB held" : "LB up", speed, static_cast<double>(std::min(1.0f, std::hypot(sx, sy))),
                  static_cast<double>(std::atan2(sx, sy) * 57.29578f), l3 ? ", L3 down" : "",
                  g_strafeCrouchActive.load(std::memory_order_relaxed) ? "in" : "out", cover,
                  g_coverKeptOff.load(std::memory_order_relaxed), agents, nodes);
    Log::Detail(line);
    if (++s_lines == kTraceLineLimit)
        Log::Detailf("Acro: strafe trace: %u lines, the limit for this session; no more", kTraceLineLimit);
}

// Called from OnStateTransition (any agent, any thread): Lara's state changes while the trace runs.
void TraceStrafeTransition(void* agent, const std::uint8_t* transition) {
    if (!g_traceOn.load(std::memory_order_relaxed) || !agent || !transition) return;
    void* lara = g_traceInstance.load(std::memory_order_relaxed);
    char line[240];
    line[0] = 0;
#if defined(_MSC_VER)
    __try {
#endif
        const auto* a = static_cast<const std::uint8_t*>(agent);
        if (!lara || *reinterpret_cast<void* const*>(a + kAgentInstance) != lara) return;
        const std::uint16_t index = *reinterpret_cast<const std::uint16_t*>(transition + kTransitionTarget);
        if (index == 0xFFFF) return;  // no state change
        auto* data = *reinterpret_cast<const std::uint8_t* const*>(a + kAgentData);
        const char* agentName = data ? *reinterpret_cast<const char* const*>(data + kAgentDataName) : nullptr;
        auto* current = *reinterpret_cast<const std::uint8_t* const*>(a + kAgentCurrentState);
        const char* to = nullptr;
        if (data && index < *reinterpret_cast<const std::uint16_t*>(data + kAgentDataNumStates)) {
            auto* const* states = *reinterpret_cast<const std::uint8_t* const* const*>(data + kAgentDataStates);
            to = states ? StateNameUnsafe(states[index]) : nullptr;
        }
        const char* via = *reinterpret_cast<const char* const*>(transition + kTransitionName);
        std::snprintf(line, sizeof(line), "%.24s: %.44s -> %.44s via '%.60s'", agentName ? agentName : "?",
                      current ? StateNameUnsafe(current) : "(none)", to ? to : "?", via ? via : "?");
#if defined(_MSC_VER)
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return;
    }
#endif
    if (line[0] && g_traceTransitions.fetch_add(1, std::memory_order_relaxed) < kTraceTransitionLimit)
        Log::Detailf("Acro: strafe trace: %s", line);
}

bool SetNamedInt(void* instance, std::uint16_t symbol, int value) {
    if (!instance || !symbol || !g_namedValueCtor || !g_namedValueSetInt) return false;
    NamedValueRef named{};
#if defined(_MSC_VER)
    __try {
#endif
        g_namedValueCtor(&named, instance, &symbol);
        g_namedValueSetInt(&named, value);
#if defined(_MSC_VER)
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
#endif
    return true;
}

// An instance named value as an int; *found false when the instance does not have it.
int ReadNamedInt(void* instance, std::uint16_t symbol, bool* found) {
    *found = false;
    if (!instance || !symbol || !g_namedValueCtor || !g_namedValueComponent || !g_namedValueSetGet || !g_variantCastToInt)
        return 0;
    NamedValueRef named{};
    int value = 0;
#if defined(_MSC_VER)
    __try {
#endif
        g_namedValueCtor(&named, instance, &symbol);
        if (named.system != 2) return 0;
        std::uint8_t* component = g_namedValueComponent(&named);
        if (!component) return 0;
        const void* variant = g_namedValueSetGet(component + kNamedValueSetOffset, &named.symbol, false);
        if (!variant) return 0;
        value = g_variantCastToInt(variant);
        *found = true;
#if defined(_MSC_VER)
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        *found = false;
        return 0;
    }
#endif
    return value;
}

// What PlayerCombatStatus::SetStatus would write to ApplyCombatRun now.
int GameCombatRun(void* instance, int* statusOut, int* forcedOut) {
    bool found = false;
    const int forced = ReadNamedInt(instance, g_symCombatRunForced ? *g_symCombatRunForced : 0, &found);
    *forcedOut = found ? forced : 0;
    bool statusFound = false;
    *statusOut = ReadNamedInt(instance, g_symCombatStatus ? *g_symCombatStatus : 0, &statusFound);
    if (!found) return 0;
    if (forced != -1) return forced;
    return *statusOut == 1 || *statusOut == 2 ? 1 : 0;
}

// Game thread, every frame: Lara's omnidirectional named values follow LB (written again every 250 ms while held, in
// case the game clears them; cleared once when LB is let go or the setting is turned off).
void UpdateOmniValuesGameThread() {
    if (!g_omniValuesAvailable.load(std::memory_order_relaxed)) return;
    const bool want = g_lbStrafe.load(std::memory_order_relaxed) && g_padHooks.load(std::memory_order_relaxed) &&
                      g_strafeHeld.load(std::memory_order_relaxed);
    OmniValues& v = g_omniValues;
    const ULONGLONG now = GetTickCount64();
    if (!want && !v.on) return;
    void* agent = CombatAir::LaraPrimaryAgent();
    void* instance = nullptr;
#if defined(_MSC_VER)
    __try {
#endif
        instance = agent ? *reinterpret_cast<void**>(static_cast<std::uint8_t*>(agent) + kAgentInstance) : nullptr;
#if defined(_MSC_VER)
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        instance = nullptr;
    }
#endif
    if (v.on && v.instance != instance) {  // another Lara (level load): the old values went with the old instance
        v.on = false;
        v.instance = nullptr;
        if (!want) return;
    }
    if (want == v.on && (!want || now - v.lastMs < 250)) return;
    if (!instance) return;
    const std::uint16_t forceOn = g_symForceOn ? *g_symForceOn : 0, allows = g_symAllows ? *g_symAllows : 0;
    const bool ok1 = SetNamedBool(instance, kSymAnimOmnidirectionalMovement, want);
    const bool ok2 = SetNamedBool(instance, allows, want);
    const bool ok3 = SetNamedBool(instance, forceOn, want);
    // no combat run while strafing (its graph pipe has no omnidirectional switch); back to the game's value after
    if (g_combatRunAvailable.load(std::memory_order_relaxed)) {
        const std::uint16_t apply = *g_symCombatRunApply;
        static int s_combatRunLogs = 0;
        if (want) {
            bool found = false;
            const int before = want != v.on ? ReadNamedInt(instance, apply, &found) : 0;
            SetNamedInt(instance, apply, 0);
            if (want != v.on && before != 0 && s_combatRunLogs++ < 12)
                Log::Detailf("Acro: LB strafe in a combat encounter: combat run off while LB is held (it was %d)", before);
        } else {
            int status = 0, forced = 0;
            const int value = GameCombatRun(instance, &status, &forced);
            SetNamedInt(instance, apply, value);
            if (value && s_combatRunLogs++ < 12)
                Log::Detailf("Acro: LB strafe released: combat run back to %d (combat status %d, forced %d)", value, status, forced);
        }
    }
    g_omniValueWrites.fetch_add(1, std::memory_order_relaxed);
    const bool changed = want != v.on;
    v.on = want;
    v.instance = want ? instance : nullptr;
    v.lastMs = now;
    if (!v.loggedNames) {
        v.loggedNames = true;
        Log::Detailf("Acro: LB strafe named values: %u '%s', %u '%s', %u '%s'", kSymAnimOmnidirectionalMovement,
                    OmniSymbolName(kSymAnimOmnidirectionalMovement), allows, OmniSymbolName(allows), forceOn,
                    OmniSymbolName(forceOn));
        if (g_combatRunAvailable.load())
            Log::Detailf("Acro: combat run named values: %u '%s', %u '%s', %u '%s'", *g_symCombatRunApply,
                        OmniSymbolName(*g_symCombatRunApply), *g_symCombatRunForced, OmniSymbolName(*g_symCombatRunForced),
                        *g_symCombatStatus, OmniSymbolName(*g_symCombatStatus));
    }
    if (changed && v.logs++ < 16)
        Log::Detailf("Acro: LB strafe named values %s for Lara %p (writes %s%s%s); the game's own omni / allowed now: %d / %d",
                    want ? "on" : "off", instance, ok1 ? "ok" : "FAILED", ok2 ? "" : ", allows FAILED",
                    ok3 ? "" : ", forced_on FAILED", NativeOmniAnswer(agent, 0), NativeOmniAnswer(agent, 1));
}

// ---------- acrobatic climb-up ----------
void RestoreClimbEntries() {
    AnimEntry* entries = g_patchedEntries.load();
    const AnimEntry* original = g_originalEntries;
    g_climbActive.store(false);
    if (!entries || !original) return;
    for (std::size_t i = 0; i < g_defCount; ++i) {
        const int e = g_defs[i].climb ? g_climbEntry[i] : -1;
        if (e >= 0 && static_cast<std::uint32_t>(e) < g_originalCount)
            WriteEntry(&entries[e], original[e].animId, original[e].animNum, original[e].fx);
    }
}

void DisarmClimb(const char* status) {
    if (g_climbActive.load()) RestoreClimbEntries();
    g_climbArmed.store(false);
    std::snprintf(g_climbStatus, sizeof(g_climbStatus), "%s", status);
}

// Finds the pull-up state in Lara's primary agent data (again when the agent or its data change).
void ArmClimbGameThread(void* agent) {
    int clips = 0;
    for (std::size_t i = 0; i < g_defCount; ++i) if (g_defs[i].climb && g_climbEntry[i] >= 0 && g_clips[i].id >= 0) ++clips;
    if (!clips) {
        DisarmClimb("no climb clips (see ROTTRFramework.log)");
        return;
    }
    if (!agent) {
        DisarmClimb("waiting for Lara");
        return;
    }
    const std::uint8_t* agentData = nullptr;
    int idx = -1;
    const std::uint8_t* state = nullptr;
#if defined(_MSC_VER)
    __try {
#endif
        agentData = *reinterpret_cast<const std::uint8_t* const*>(static_cast<std::uint8_t*>(agent) + kAgentData);
        if (agentData && (agent != g_climbAgent.load() || agentData != g_climbAgentData || g_idxClimb.load() < 0)) {
            const std::uint16_t numStates = *reinterpret_cast<const std::uint16_t*>(agentData + kAgentDataNumStates);
            auto* const* states = *reinterpret_cast<const std::uint8_t* const* const*>(agentData + kAgentDataStates);
            for (int k = 0; states && k < numStates && k < 8000; ++k) {
                const char* name = StateNameUnsafe(states[k]);
                if (name && std::strcmp(name, kStateClimbName) == 0) {
                    idx = k;
                    state = states[k];
                    break;
                }
            }
        } else if (agentData) {
            idx = g_idxClimb.load();
            state = g_stateClimb.load();
        }
#if defined(_MSC_VER)
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        idx = -1;
    }
#endif
    if (idx < 0 || !state) {
        DisarmClimb("unavailable (pull-up state not found)");
        return;
    }
    if (agent != g_climbAgent.load() || agentData != g_climbAgentData) {
        if (g_climbActive.load()) RestoreClimbEntries();
        g_climbAgentData = agentData;
        g_idxClimb.store(idx);
        g_stateClimb.store(state);
        g_climbAgent.store(agent);
        Log::DetailOncef("Acro: acrobatic climb-up ready (agent %p, state %s=%d, %d clips; hold A / Space when climbing up)", agent,
                    kStateClimbName, idx, clips);
    }
    // A climb that ended without the hook seeing it (death, reload): put the game's clips back after a while.
    if (g_climbActive.load() && GetTickCount64() - g_climbActiveSinceMs.load() > 5000) {
        const std::uint8_t* current = nullptr;
#if defined(_MSC_VER)
        __try {
#endif
            current = *reinterpret_cast<const std::uint8_t* const*>(static_cast<std::uint8_t*>(agent) + kAgentCurrentState);
#if defined(_MSC_VER)
        } __except (EXCEPTION_EXECUTE_HANDLER) {
        }
#endif
        if (current != g_stateClimb.load()) RestoreClimbEntries();
    }
    g_climbArmed.store(true);
    std::snprintf(g_climbStatus, sizeof(g_climbStatus), "ready (%d acrobatic climb-ups so far)", g_climbCount.load());
}

// Before Lara's primary agent enters the pull-up state: our clips while the jump button is held; leaving it: the
// game's own again.
void OnClimbTransition(void* agent, const std::uint8_t* transition) {
    if (!g_climbArmed.load(std::memory_order_relaxed) || !agent || agent != g_climbAgent.load(std::memory_order_relaxed) ||
        !transition)
        return;
    AnimEntry* entries = g_patchedEntries.load(std::memory_order_acquire);
    const AnimEntry* original = g_originalEntries;
    if (!entries || !original) return;
    int target = -1;
    const std::uint8_t* current = nullptr;
    const char* via = nullptr;
#if defined(_MSC_VER)
    __try {
#endif
        target = *reinterpret_cast<const std::uint16_t*>(transition + kTransitionTarget);
        current = *reinterpret_cast<const std::uint8_t* const*>(static_cast<std::uint8_t*>(agent) + kAgentCurrentState);
        via = *reinterpret_cast<const char* const*>(transition + kTransitionName);
#if defined(_MSC_VER)
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return;
    }
#endif
    if (target == 0xFFFF) return;
    if (target == g_idxClimb.load(std::memory_order_relaxed)) {
        const unsigned heldMs = CombatAir::JumpButtonHeldMs();
        const bool held = heldMs >= static_cast<unsigned>(g_climbHoldMs.load());
        // held long enough: the wheel ([Climb]); otherwise the vault ([ClimbTap]); either off: the game's climb-up
        const bool wanted = held ? g_climbWheel.load() : g_climbVault.load();
        if (g_climbActive.load()) RestoreClimbEntries();
        if (!wanted) return;
        int written = 0;
        for (std::size_t i = 0; i < g_defCount; ++i) {
            const int e = g_defs[i].climb && g_defs[i].climbTap != held ? g_climbEntry[i] : -1;
            if (e < 0 || static_cast<std::uint32_t>(e) >= g_originalCount || g_clips[i].id < 0) continue;
            const auto id = static_cast<std::int16_t>(g_clips[i].id);
            WriteEntry(&entries[e], id, id, g_clipSounds[i]);
            ++written;
        }
        if (!written) return;
        g_climbActive.store(true);
        g_climbActiveSinceMs.store(GetTickCount64());
        const int n = g_climbCount.fetch_add(1) + 1;
        if (g_climbLogs.fetch_add(1) < 40)
            Log::Detailf("Acro: %s climb-up #%d (jump button held %u ms; via %s, %d entries)",
                        held ? "acrobatic (wheel)" : "Black Widow vault", n, heldMs, via ? via : "?", written);
        return;
    }
    if (g_climbActive.load() && current == g_stateClimb.load()) RestoreClimbEntries();  // leaving the pull-up
}

// ---------- Black Widow high hurdle ----------
void RestoreHurdleEntries() {
    AnimEntry* entries = g_patchedEntries.load();
    const AnimEntry* original = g_originalEntries;
    g_hurdleActive.store(false);
    if (!entries || !original) return;
    for (std::size_t i = 0; i < g_defCount; ++i) {
        if (!g_defs[i].hurdle) continue;
        for (int k = 0; k < kMaxEvadeTargets; ++k) {
            const int e = g_hurdleEntry[i][k];
            if (e >= 0 && static_cast<std::uint32_t>(e) < g_originalCount)
                WriteEntry(&entries[e], original[e].animId, original[e].animNum, original[e].fx);
        }
    }
}

void DisarmHurdle(const char* status) {
    if (g_hurdleActive.load()) RestoreHurdleEntries();
    g_hurdleArmed.store(false);
    std::snprintf(g_hurdleStatus, sizeof(g_hurdleStatus), "%s", status);
}

// Finds the two hurdle states in Lara's primary agent data (again when the agent or its data change).
void ArmHurdleGameThread(void* agent) {
    int targets = 0;
    for (std::size_t i = 0; i < g_defCount; ++i)
        if (g_defs[i].hurdle && g_clips[i].id >= 0)
            for (int k = 0; k < kMaxEvadeTargets; ++k) targets += g_hurdleEntry[i][k] >= 0;
    if (!targets) {
        DisarmHurdle("no hurdle clips (see ROTTRFramework.log)");
        return;
    }
    if (!agent) {
        DisarmHurdle("waiting for Lara");
        return;
    }
    const std::uint8_t* agentData = nullptr;
    int attach = -1, onto = -1;
    const std::uint8_t* attachState = nullptr;
    const std::uint8_t* ontoState = nullptr;
#if defined(_MSC_VER)
    __try {
#endif
        agentData = *reinterpret_cast<const std::uint8_t* const*>(static_cast<std::uint8_t*>(agent) + kAgentData);
        if (agentData && (agent != g_hurdleAgent.load() || agentData != g_hurdleAgentData || g_idxHurdleAttach.load() < 0)) {
            const std::uint16_t numStates = *reinterpret_cast<const std::uint16_t*>(agentData + kAgentDataNumStates);
            auto* const* states = *reinterpret_cast<const std::uint8_t* const* const*>(agentData + kAgentDataStates);
            for (int k = 0; states && k < numStates && k < 8000 && (attach < 0 || onto < 0); ++k) {
                const char* name = StateNameUnsafe(states[k]);
                if (!name) continue;
                if (attach < 0 && std::strcmp(name, kStateHurdleAttachName) == 0) {
                    attach = k;
                    attachState = states[k];
                } else if (onto < 0 && std::strcmp(name, kStateHurdleOntoName) == 0) {
                    onto = k;
                    ontoState = states[k];
                }
            }
        } else if (agentData) {
            attach = g_idxHurdleAttach.load();
            onto = g_idxHurdleOnto.load();
            attachState = g_stateHurdleAttach.load();
            ontoState = g_stateHurdleOnto.load();
        }
#if defined(_MSC_VER)
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        attach = onto = -1;
    }
#endif
    if (attach < 0 || onto < 0 || !attachState || !ontoState) {
        DisarmHurdle("unavailable (hurdle states not found)");
        return;
    }
    if (agent != g_hurdleAgent.load() || agentData != g_hurdleAgentData) {
        if (g_hurdleActive.load()) RestoreHurdleEntries();
        g_hurdleAgentData = agentData;
        g_idxHurdleAttach.store(attach);
        g_idxHurdleOnto.store(onto);
        g_stateHurdleAttach.store(attachState);
        g_stateHurdleOnto.store(ontoState);
        g_hurdleAgent.store(agent);
        Log::DetailOncef("Acro: Black Widow high hurdle ready (agent %p, states %s=%d, %s=%d, %d entries)", agent,
                    kStateHurdleAttachName, attach, kStateHurdleOntoName, onto, targets);
    }
    // A climb that ended without the hook seeing it (death, reload): put the game's clips back after a while.
    if (g_hurdleActive.load() && GetTickCount64() - g_hurdleActiveSinceMs.load() > 6000) {
        const std::uint8_t* current = nullptr;
#if defined(_MSC_VER)
        __try {
#endif
            current = *reinterpret_cast<const std::uint8_t* const*>(static_cast<std::uint8_t*>(agent) + kAgentCurrentState);
#if defined(_MSC_VER)
        } __except (EXCEPTION_EXECUTE_HANDLER) {
        }
#endif
        if (current != g_stateHurdleAttach.load() && current != g_stateHurdleOnto.load()) RestoreHurdleEntries();
    }
    g_hurdleArmed.store(true);
    std::snprintf(g_hurdleStatus, sizeof(g_hurdleStatus), "ready (%d Black Widow climbs onto high ledges so far)",
                  g_hurdleCount.load());
}

// Before Lara's primary agent enters hurdle_attach_from_ground_high: the Black Widow clips in the attach and onto
// entries; leaving hurdle_onto (or the attach going anywhere but hurdle_onto): the game's own again.
void OnHurdleTransition(void* agent, const std::uint8_t* transition) {
    if (!g_hurdleArmed.load(std::memory_order_relaxed) || !agent || agent != g_hurdleAgent.load(std::memory_order_relaxed) ||
        !transition)
        return;
    AnimEntry* entries = g_patchedEntries.load(std::memory_order_acquire);
    const AnimEntry* original = g_originalEntries;
    if (!entries || !original) return;
    int target = -1;
    const std::uint8_t* current = nullptr;
    const char* via = nullptr;
#if defined(_MSC_VER)
    __try {
#endif
        target = *reinterpret_cast<const std::uint16_t*>(transition + kTransitionTarget);
        current = *reinterpret_cast<const std::uint8_t* const*>(static_cast<std::uint8_t*>(agent) + kAgentCurrentState);
        via = *reinterpret_cast<const char* const*>(transition + kTransitionName);
#if defined(_MSC_VER)
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return;
    }
#endif
    if (target == 0xFFFF) return;
    if (target == g_idxHurdleAttach.load(std::memory_order_relaxed)) {
        int written = 0;
        for (std::size_t i = 0; i < g_defCount; ++i) {
            if (!g_defs[i].hurdle || g_clips[i].id < 0) continue;
            const auto id = static_cast<std::int16_t>(g_clips[i].id);
            for (int k = 0; k < kMaxEvadeTargets; ++k) {
                const int e = g_hurdleEntry[i][k];
                if (e < 0 || static_cast<std::uint32_t>(e) >= g_originalCount) continue;
                WriteEntry(&entries[e], id, id, g_hurdleFx[i][k]);
                ++written;
            }
        }
        if (!written) return;
        g_hurdleActive.store(true);
        g_hurdleActiveSinceMs.store(GetTickCount64());
        const int n = g_hurdleCount.fetch_add(1) + 1;
        if (g_hurdleLogs.fetch_add(1) < 40)
            Log::Detailf("Acro: Black Widow high hurdle #%d (via %s, %d entries)", n, via ? via : "?", written);
        return;
    }
    if (!g_hurdleActive.load()) return;
    if (target == g_idxHurdleOnto.load(std::memory_order_relaxed)) return;  // the attach goes on to the stand-up
    if (current == g_stateHurdleAttach.load() || current == g_stateHurdleOnto.load()) {
        RestoreHurdleEntries();
        if (g_hurdleLogs.fetch_add(1) < 40) Log::Detailf("Acro: Black Widow high hurdle over (via %s)", via ? via : "?");
    }
}

void TickUnsafe() {
    if (g_reloadRequested.exchange(false)) ReloadClipsGameThread();
    if (g_reapplyRequested.exchange(false) && g_patched) {
        RestoreGameThread();  // the flow below applies again with the new settings
        Log::Detailf("Acro: settings changed (evades %s, roll chain %s, sounds %s)", g_enabled.load() ? "on" : "off",
                    g_rollChain.load() ? "on" : "off", g_flipSounds.load() ? "on" : "off");
    }
    const bool evades = g_enabled.load(std::memory_order_relaxed);
    const bool chain = g_rollChain.load(std::memory_order_relaxed) && CombatAir::TransitionHookActive();
    const bool climb = (g_climbWheel.load(std::memory_order_relaxed) || g_climbVault.load(std::memory_order_relaxed)) &&
                       CombatAir::TransitionHookActive();
    const bool hurdle = g_bwHurdle.load(std::memory_order_relaxed) && CombatAir::TransitionHookActive();
    void* agent = chain || climb || hurdle ? CombatAir::LaraPrimaryAgent() : nullptr;
    if (!climb)
        DisarmClimb((g_climbWheel.load() || g_climbVault.load()) && !CombatAir::TransitionHookActive()
                        ? "unavailable (no transition hook)"
                        : "off");
    if (!hurdle)
        DisarmHurdle(g_bwHurdle.load() && !CombatAir::TransitionHookActive() ? "unavailable (no transition hook)" : "off");
    {
        const bool strafe = g_lbStrafe.load(std::memory_order_relaxed) && g_padHooks.load(std::memory_order_relaxed);
        UpdateOmniConditionGameThread(agent ? agent : CombatAir::LaraPrimaryAgent(), strafe);
        UpdateSprintConditionGameThread(strafe);
        const bool held = g_strafeHeld.load(std::memory_order_relaxed);
        static bool s_wasHeld = false;
        if (held != s_wasHeld && g_strafeLogs.fetch_add(1) < 20)
            Log::Detailf("Acro: LB strafe %s (%u answers so far)", held ? "held" : "released", g_omniAnswers.load());
        s_wasHeld = held;
    }
    // State control patch first: off when the chain is off; a new agent / agent data gets its own patch.
    if (g_chainSc.agentData) {
        if (!chain) {
            RestoreChainScGameThread();
        } else if (agent) {
            std::uint8_t* agentData = nullptr;
#if defined(_MSC_VER)
            __try {
#endif
                agentData = *reinterpret_cast<std::uint8_t**>(static_cast<std::uint8_t*>(agent) + kAgentData);
#if defined(_MSC_VER)
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                agentData = nullptr;
            }
#endif
            if (agent != g_chainAgent.load() || agentData != g_chainSc.agentData) {
                Log::DetailOncef("Acro: Lara's state control changed; the roll chain patches it again");
                g_chainArmed.store(false);
                g_chainSc = ChainSc{};  // the old data may be gone: do not write to it
                g_chainAgent.store(nullptr);
            }
        }
    }
    const bool strafeCrouch = g_strafeCrouch.load(std::memory_order_relaxed) && g_lbStrafe.load(std::memory_order_relaxed);
    const bool jump = g_bwJump.load(std::memory_order_relaxed);
    if (!evades && !chain && !climb && !hurdle && !strafeCrouch && !jump) {
        if (g_patched) RestoreGameThread();
        SetStatus(g_patched ? "restoring" : "off");
        std::snprintf(g_chainStatus, sizeof(g_chainStatus), "%s",
                      g_rollChain.load() && !CombatAir::TransitionHookActive() ? "unavailable (no transition hook)" : "off");
        g_statusPatched.store(false);
        return;
    }
    EnsureClipsGameThread();
    if (!g_statusClips.load()) {
        SetStatus("no clips loaded (see ROTTRFramework.log)");
        return;
    }
    std::uint8_t* library = FindLibrary(g_laraKey);
    AnimEntry* entries = nullptr;
    std::uint32_t count = 0;
    const char* name = nullptr;
    bool found = library && ReadLibrary(library, entries, count, name) && NameEquals(name, kLaraLibraryName);
    if (!found) {
        const ULONGLONG now = GetTickCount64();
        std::uint32_t key = 0;
        if (now >= g_nextScanMs && ScanRegistryForLara(key, library)) {
            if (key != g_laraKey) Log::Detailf("Acro: Lara's animation library found under registry key %u", key);
            g_laraKey = key;
            found = ReadLibrary(library, entries, count, name);
        }
        if (!found) {
            g_nextScanMs = now + 3000ull;
            SetStatus("waiting for Lara's animation library");
            g_statusPatched.store(false);
            g_chainArmed.store(false);
            DisarmClimb("waiting for Lara's animation library");
            DisarmHurdle("waiting for Lara's animation library");
            return;
        }
    }
    if (!g_keyLogged) {
        g_keyLogged = true;
        Log::Detailf("Acro: Lara's animation library %p (key %u, %u entries)", library, g_laraKey, count);
    }
    const bool current = g_patched && library == g_library && entries == g_patchedEntries.load() &&
                         g_patchedEvades == evades && g_patchedChain == chain && g_patchedJump == jump;
    if (!current) {
        // First time, the library was reloaded (new memory) or the features changed since the last patch.
        if (g_patched && library != g_library) Log::Detail("Acro: Lara's animation library was reloaded; applying again");
        if (g_patched && library == g_library && entries == g_patchedEntries.load()) RestoreGameThread();
        else g_patched = false;
        g_chainArmed.store(false);
        g_climbArmed.store(false);
        g_climbActive.store(false);
        g_hurdleArmed.store(false);
        g_hurdleActive.store(false);
        g_strafeCrouchActive.store(false);
        ApplyPatchGameThread(library, entries, count, evades, chain, jump);
    }
    g_statusPatched.store(g_patched);
    g_statusLibraryCount.store(g_patched ? static_cast<int>(g_originalCount) : static_cast<int>(count));
    g_statusRedirected.store(g_redirected);
    SetStatus(g_patched ? "active" : "could not extend the library");
    if (climb) {
        if (g_patched) ArmClimbGameThread(agent);
        else DisarmClimb("waiting");
    }
    if (hurdle) {
        if (g_patched) ArmHurdleGameThread(agent);
        else DisarmHurdle("waiting");
    }
    if (!chain) return;
    if (!g_patched || !g_chainEntriesOk || g_chainLength == 0) {
        g_chainArmed.store(false);
        std::snprintf(g_chainStatus, sizeof(g_chainStatus), "%s", g_patched ? "unavailable (library entries differ)" : "waiting");
        return;
    }
    if (!agent) {
        g_chainArmed.store(false);
        std::snprintf(g_chainStatus, sizeof(g_chainStatus), "waiting for Lara");
        return;
    }
    if (!g_chainSc.agentData && !ApplyChainScGameThread(agent)) {
        g_chainArmed.store(false);
        return;
    }
    const float factor = static_cast<float>(g_chainDistance.load()) / 100.0f;
    if (g_chainSc.speed[0] && factor != g_chainSc.speedFactor) {
#if defined(_MSC_VER)
        __try {
#endif
            for (int k = 0; k < 4; ++k) if (g_chainSc.speed[k]) *g_chainSc.speed[k] = g_chainSc.speedOriginal[k] * factor;
            g_chainSc.speedFactor = factor;
#if defined(_MSC_VER)
        } __except (EXCEPTION_EXECUTE_HANDLER) {
        }
#endif
        Log::Detailf("Acro: roll chain distance %d%% (state push %.1f)", g_chainDistance.load(), g_chainSc.speedOriginal[0] * factor);
    }
    if (!g_chainArmed.load()) {
        ResetChainEntries();
        g_chainStep.store(-1);
        g_chainArmed.store(true);
    }
    const int step = g_chainStep.load();
    if (step >= 0)
        std::snprintf(g_chainStatus, sizeof(g_chainStatus), "in a chain, move %d", step + 1);
    else
        std::snprintf(g_chainStatus, sizeof(g_chainStatus), "ready (longest chain so far: %d)", g_chainLongest.load());
}

}  // namespace

namespace Acro {
bool Initialize() {
    if (!GameBuild::IsSupportedRetail()) return false;
    g_base = GameBuild::Base();
    LoadSettings();
    const bool resolveOk = GameBuild::VerifyBytes(kRvaResolveAnimation, kResolveAnimationSig, sizeof(kResolveAnimationSig));
    const bool limitOk = GameBuild::VerifyBytes(kRvaGetKeylistLimit, kGetKeylistLimitSig, sizeof(kGetKeylistLimitSig));
    const bool registryOk = GameBuild::VerifyBytes(kRvaGetKeylistRegistry, kGetKeylistRegistrySig, sizeof(kGetKeylistRegistrySig));
    std::uintptr_t registryRva = 0;
    if (registryOk) {
        const std::int32_t rel = *reinterpret_cast<const std::int32_t*>(g_base + kRvaGetKeylistRegistry + 3);
        registryRva = static_cast<std::uintptr_t>(static_cast<std::intptr_t>(kRvaGetKeylistRegistry + 7) + rel);
    }
    g_available = resolveOk && limitOk && registryOk && registryRva == kRvaRegistry;
    if (g_available) {
        g_refCounts = reinterpret_cast<std::uint16_t*>(g_base + kRvaRefCounts);
        g_animByNumber = reinterpret_cast<void**>(g_base + kRvaAnimByNumber);
        g_registry = reinterpret_cast<std::uint8_t* const*>(g_base + kRvaRegistry);
    }
    Log::Detailf("Acro: %s (tracker %s, id limit %s, library registry %s); Black Widow evades %s",
                g_available ? "ready" : "unavailable", resolveOk ? "ok" : "mismatch", limitOk ? "ok" : "mismatch",
                registryOk && registryRva == kRvaRegistry ? "ok" : "mismatch", g_enabled.load() ? "on" : "off");
    Log::Detailf("Acro: roll chain %s", g_rollChain.load() ? "on" : "off");
    int padHooks = 0;
    for (int k = 0; k < 2; ++k) {
        const wchar_t* name = k == 0 ? L"xinput1_3.dll" : L"xinput9_1_0.dll";
        HMODULE module = LoadLibraryW(name);
        void* fn = module ? reinterpret_cast<void*>(GetProcAddress(module, "XInputGetState")) : nullptr;
        if (!fn) continue;
        void* hook = k == 0 ? reinterpret_cast<void*>(&HookXInput13) : reinterpret_cast<void*>(&HookXInput910);
        void** original = k == 0 ? reinterpret_cast<void**>(&g_origXInput13) : reinterpret_cast<void**>(&g_origXInput910);
        if (MH_CreateHook(fn, hook, original) == MH_OK) ++padHooks;
    }
    g_padHooks.store(padHooks > 0);
    {
        int coverHooks = 0;
        const struct { std::uintptr_t rva; const std::uint8_t* sig; std::size_t size; void* hook; void** original; } covers[] = {
            {kRvaSetLowCover, kSetLowCoverSig, sizeof(kSetLowCoverSig), reinterpret_cast<void*>(&HookSetLowCover),
             reinterpret_cast<void**>(&g_origSetLowCover)},
            {kRvaSetStandingCover, kSetStandingCoverSig, sizeof(kSetStandingCoverSig),
             reinterpret_cast<void*>(&HookSetStandingCover), reinterpret_cast<void**>(&g_origSetStandingCover)},
            {kRvaSetCornerLean, kSetCornerLeanSig, sizeof(kSetCornerLeanSig), reinterpret_cast<void*>(&HookSetCornerLean),
             reinterpret_cast<void**>(&g_origSetCornerLean)},
        };
        for (const auto& c : covers)
            if (GameBuild::VerifyBytes(c.rva, c.sig, c.size) &&
                MH_CreateHook(reinterpret_cast<void*>(g_base + c.rva), c.hook, c.original) == MH_OK)
                ++coverHooks;
        g_coverHooks.store(coverHooks);
        Log::Detailf("Acro: LB strafe near cover: %d of 3 cover hooks (Lara stands to strafe)", coverHooks);
    }
    if (GameBuild::VerifyBytes(kRvaFragmentUpdate, kFragmentUpdateSig, sizeof(kFragmentUpdateSig)) &&
        MH_CreateHook(reinterpret_cast<void*>(g_base + kRvaFragmentUpdate), reinterpret_cast<void*>(&HookFragmentUpdate),
                      reinterpret_cast<void**>(&g_origFragmentUpdate)) == MH_OK)
        g_fragmentHook.store(true);
    Log::Detailf("Acro: LB strafe trace %s (AnimFragmentNode::Update hook %s)", g_strafeTrace.load() ? "on" : "off",
                g_fragmentHook.load() ? "ready" : "unavailable");
    const bool namedOk = GameBuild::VerifyBytes(kRvaNamedValueCtor, kNamedValueCtorSig, sizeof(kNamedValueCtorSig)) &&
                         GameBuild::VerifyBytes(kRvaNamedValueSetBool, kNamedValueSetBoolSig, sizeof(kNamedValueSetBoolSig));
    const bool symbolsOk =
        GameBuild::VerifyBytes(kRvaMoveOmniForceOnLoad, kMoveOmniForceOnLoadSig, sizeof(kMoveOmniForceOnLoadSig)) &&
        GameBuild::VerifyBytes(kRvaMoveOmniAllowsLoad, kMoveOmniAllowsLoadSig, sizeof(kMoveOmniAllowsLoadSig));
    if (namedOk && symbolsOk) {
        g_namedValueCtor = reinterpret_cast<NamedValueCtorFn>(g_base + kRvaNamedValueCtor);
        g_namedValueSetBool = reinterpret_cast<NamedValueSetBoolFn>(g_base + kRvaNamedValueSetBool);
        const auto target = [](std::uintptr_t rva) {  // movzx eax,word ptr [rip+rel32] (7 bytes)
            return g_base + rva + 7 + *reinterpret_cast<const std::int32_t*>(g_base + rva + 3);
        };
        g_symForceOn = reinterpret_cast<const std::uint16_t*>(target(kRvaMoveOmniForceOnLoad));
        g_symAllows = reinterpret_cast<const std::uint16_t*>(target(kRvaMoveOmniAllowsLoad));
        g_omniValuesAvailable.store(true);
    }
    if (GameBuild::VerifyBytes(kRvaOmniSymbolCStr, kOmniSymbolCStrSig, sizeof(kOmniSymbolCStrSig)))
        g_omniSymbolCStr = reinterpret_cast<SymbolCStrFn>(g_base + kRvaOmniSymbolCStr);
    const bool combatRunOk =
        g_omniValuesAvailable.load() &&
        GameBuild::VerifyBytes(kRvaNamedValueSetInt, kNamedValueSetIntSig, sizeof(kNamedValueSetIntSig)) &&
        GameBuild::VerifyBytes(kRvaNamedValueComponent, kNamedValueComponentSig, sizeof(kNamedValueComponentSig)) &&
        GameBuild::VerifyBytes(kRvaNamedValueSetGet, kNamedValueSetGetSig, sizeof(kNamedValueSetGetSig)) &&
        GameBuild::VerifyBytes(kRvaVariantCastToInt, kVariantCastToIntSig, sizeof(kVariantCastToIntSig)) &&
        GameBuild::VerifyBytes(kRvaCombatRunForcedLoad, kCombatRunForcedLoadSig, sizeof(kCombatRunForcedLoadSig)) &&
        GameBuild::VerifyBytes(kRvaCombatRunApplyLoad, kCombatRunApplyLoadSig, sizeof(kCombatRunApplyLoadSig)) &&
        GameBuild::VerifyBytes(kRvaCombatStatusLoad, kCombatStatusLoadSig, sizeof(kCombatStatusLoadSig));
    if (combatRunOk) {
        g_namedValueSetInt = reinterpret_cast<NamedValueSetIntFn>(g_base + kRvaNamedValueSetInt);
        g_namedValueComponent = reinterpret_cast<NamedValueComponentFn>(g_base + kRvaNamedValueComponent);
        g_namedValueSetGet = reinterpret_cast<NamedValueSetGetFn>(g_base + kRvaNamedValueSetGet);
        g_variantCastToInt = reinterpret_cast<VariantCastToIntFn>(g_base + kRvaVariantCastToInt);
        const auto target = [](std::uintptr_t rva) {  // movzx eax,word ptr [rip+rel32] (7 bytes)
            return g_base + rva + 7 + *reinterpret_cast<const std::int32_t*>(g_base + rva + 3);
        };
        g_symCombatRunForced = reinterpret_cast<const std::uint16_t*>(target(kRvaCombatRunForcedLoad));
        g_symCombatRunApply = reinterpret_cast<const std::uint16_t*>(target(kRvaCombatRunApplyLoad));
        g_symCombatStatus = reinterpret_cast<const std::uint16_t*>(target(kRvaCombatStatusLoad));
        g_combatRunAvailable.store(true);
    }
    Log::Detailf("Acro: LB strafe in combat encounters (combat run off while held) %s", combatRunOk ? "ready" : "unavailable");
    Log::Detailf("Acro: LB strafe %s (%d game pad hooks: xinput1_3 / xinput9_1_0 XInputGetState; omni animation values %s: "
                "named value code %s, omni symbols %s)", g_lbStrafe.load() ? "on" : "off", padHooks,
                g_omniValuesAvailable.load() ? "ready" : "unavailable", namedOk ? "ok" : "mismatch", symbolsOk ? "ok" : "mismatch");
    return g_available;
}

void TickGameThread() {
    if (!g_available) return;
#if defined(_MSC_VER)
    __try {
#endif
        UpdateStrafeCrouchGameThread();
        ClearCoverForStrafeGameThread();
        UpdateOmniValuesGameThread();
        StrafeTraceGameThread();
        UpdateCombatOmniFillGameThread();
#if defined(_MSC_VER)
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        static bool logged = false;
        if (!logged) {
            logged = true;
            Log::Write("Acro: LB strafe named values raised an exception; skipped");
        }
    }
#endif
    const ULONGLONG now = GetTickCount64();
    if (now < g_nextTickMs) return;
    g_nextTickMs = now + 500ull;
#if defined(_MSC_VER)
    __try {
#endif
        TickUnsafe();
#if defined(_MSC_VER)
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        static bool logged = false;
        if (!logged) {
            logged = true;
            Log::Write("Acro: tick raised an exception; skipped");
        }
    }
#endif
}

// Runs inside the game's SCAgent::DoTransition (any agent, any thread the state control runs on): before Lara's
// primary agent enters ground_evade / ground_evade_dive, the entries that state's graph node plays get the chain's
// next clip; leaving the chain puts the game's own clips back.
void OnStateTransition(void* agent, const std::uint8_t* transition) {
    TraceStrafeTransition(agent, transition);
    OnClimbTransition(agent, transition);
    OnHurdleTransition(agent, transition);
    if (!g_chainArmed.load(std::memory_order_relaxed) || !agent || agent != g_chainAgent.load(std::memory_order_relaxed) ||
        !transition || g_chainLength == 0)
        return;
    AnimEntry* entries = g_patchedEntries.load(std::memory_order_acquire);
    const AnimEntry* original = g_originalEntries;
    if (!entries || !original) return;
    int step = -1;
    int target = -1;
    bool fromChain = false, toChain = false, ended = false;
#if defined(_MSC_VER)
    __try {
#endif
        target = *reinterpret_cast<const std::uint16_t*>(transition + kTransitionTarget);
        if (target == 0xFFFF) return;  // no state change ("not_ended_continue" runs right after each entry)
        const auto* current = *reinterpret_cast<const std::uint8_t* const*>(static_cast<std::uint8_t*>(agent) + kAgentCurrentState);
        const bool toEvade = target == g_idxEvade.load(std::memory_order_relaxed);
        const bool toDive = target == g_idxDive.load(std::memory_order_relaxed);
        fromChain = current && (current == g_stateEvade.load(std::memory_order_relaxed) ||
                                current == g_stateDive.load(std::memory_order_relaxed));
        toChain = toEvade || toDive;
        if (toChain) {
            step = fromChain ? g_chainStep.load(std::memory_order_relaxed) + 1 : 0;
            if (step < 0) step = 0;
            const std::size_t index = g_chainOrder[static_cast<std::size_t>(step) % g_chainLength];
            const Clip& clip = g_clips[index];
            if (clip.id > 0) {
                const auto id = static_cast<std::int16_t>(clip.id);
                if (toEvade) {
                    for (std::uint32_t i = kScrambleFirst; i <= kScrambleLast; ++i) WriteEntry(&entries[i], id, id, g_chainSounds[index]);
                } else {
                    WriteEntry(&entries[kRollEntry], id, id, g_chainSounds[index]);
                }
            }
            g_chainStep.store(step, std::memory_order_relaxed);
            if (step + 1 > g_chainLongest.load(std::memory_order_relaxed)) g_chainLongest.store(step + 1, std::memory_order_relaxed);
        } else if (fromChain && g_chainStep.load(std::memory_order_relaxed) >= 0) {
            // Leaving the roll states: the node that is playing keeps its clip; later scrambles (cover) and rolls
            // get the game's own again.
            for (std::uint32_t i = kScrambleFirst; i <= kScrambleLast; ++i)
                WriteEntry(&entries[i], original[i].animId, original[i].animNum, original[i].fx);
            WriteEntry(&entries[kRollEntry], original[kRollEntry].animId, original[kRollEntry].animNum, original[kRollEntry].fx);
            step = g_chainStep.exchange(-1, std::memory_order_relaxed);
            ended = true;
        }
#if defined(_MSC_VER)
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return;
    }
#endif
    if (toChain) g_moveClip = static_cast<int>(g_chainOrder[static_cast<std::size_t>(step) % g_chainLength]);
    else if (ended) g_moveClip = -1;
}

bool AllowTransition(void* agent, const std::uint8_t* transition) {
    if (!g_chainArmed.load(std::memory_order_relaxed) || !agent || agent != g_chainAgent.load(std::memory_order_relaxed) ||
        !transition || (transition != g_chainSc.repeat && transition != g_chainSc.evadeToDive))
        return true;
    const int clip = g_moveClip;
    if (clip < 0 || clip >= static_cast<int>(g_defCount) || g_clips[clip].ready <= 0) return true;
    float frames = 0.0f;
#if defined(_MSC_VER)
    __try {
#endif
        frames = *reinterpret_cast<const float*>(static_cast<const std::uint8_t*>(agent) + kAgentFramesInState);
#if defined(_MSC_VER)
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return true;
    }
#endif
    return frames + 0.5f >= static_cast<float>(g_clips[clip].ready);
}

bool IsChainMove(void* agent) {
    if (!g_chainArmed.load(std::memory_order_relaxed) || !agent || agent != g_chainAgent.load(std::memory_order_relaxed) ||
        g_chainStep.load(std::memory_order_relaxed) < 0)
        return false;
#if defined(_MSC_VER)
    __try {
#endif
        const auto* current = *reinterpret_cast<const std::uint8_t* const*>(static_cast<std::uint8_t*>(agent) + kAgentCurrentState);
        return current && (current == g_stateEvade.load(std::memory_order_relaxed) ||
                           current == g_stateDive.load(std::memory_order_relaxed));
#if defined(_MSC_VER)
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
#endif
}

bool ChainMoveReady(void* agent, float* framesOut, int* readyOut) {
    if (!IsChainMove(agent)) return false;
    const int clip = g_moveClip;
    if (clip < 0 || clip >= static_cast<int>(g_defCount) || g_clips[clip].ready <= 0) return false;
    float frames = 0.0f;
#if defined(_MSC_VER)
    __try {
#endif
        frames = *reinterpret_cast<const float*>(static_cast<const std::uint8_t*>(agent) + kAgentFramesInState);
#if defined(_MSC_VER)
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
#endif
    if (framesOut) *framesOut = frames;
    if (readyOut) *readyOut = g_clips[clip].ready;
    return frames + 0.5f >= static_cast<float>(g_clips[clip].ready);
}

bool HoldBackWhileStrafing(void* agent, const std::uint8_t* transition) {
    if (!StrafeHeld() || !agent || !transition) return false;
    void* lara = LaraInstanceNow();
    const char* target = nullptr;
    const char* via = nullptr;
#if defined(_MSC_VER)
    __try {
#endif
        // Any of Lara's agents: the sprint itself is the Sub-State Modifiers agent's ground_sprint (default ->
        // ground_sprint via 'ground_to_sprint', logs v0.9.249..266), not a primary agent state.
        if (!lara || *reinterpret_cast<void* const*>(static_cast<std::uint8_t*>(agent) + kAgentInstance) != lara) return false;
        const std::uint16_t index = *reinterpret_cast<const std::uint16_t*>(transition + kTransitionTarget);
        auto* agentData = *reinterpret_cast<const std::uint8_t* const*>(static_cast<std::uint8_t*>(agent) + kAgentData);
        if (!agentData || index == 0xFFFF) return false;
        const std::uint16_t numStates = *reinterpret_cast<const std::uint16_t*>(agentData + kAgentDataNumStates);
        auto* const* states = *reinterpret_cast<const std::uint8_t* const* const*>(agentData + kAgentDataStates);
        if (!states || index >= numStates) return false;
        target = StateNameUnsafe(states[index]);
        via = *reinterpret_cast<const char* const*>(transition + kTransitionName);
#if defined(_MSC_VER)
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
#endif
    if (!target) return false;
    static constexpr const char* kSprintStates[] = {"ground_sprint", "ground_move_sprint", "ground_move_sprint_turn",
                                                    "ground_stairs_sprint", "combat_ranged_move_sprint"};
    for (const char* sprint : kSprintStates) {
        if (std::strcmp(target, sprint) != 0) continue;
        static std::atomic<std::uint32_t> s_logs{0};
        if (s_logs.fetch_add(1, std::memory_order_relaxed) < 8)
            Log::Detailf("Acro: LB strafe: no sprint while strafing ('%s' -> %s held back)", via ? via : "?", target);
        return true;
    }
    return false;
}

bool StrafeHeld() {
    return g_lbStrafe.load(std::memory_order_relaxed) && g_padHooks.load(std::memory_order_relaxed) &&
           g_strafeHeld.load(std::memory_order_relaxed);
}

bool AcrobaticsProtection() {
    return g_noHits.load(std::memory_order_relaxed);
}

bool PadAimHeld() {   // (v0.9.582)
    const ULONGLONG at = g_padReadMs.load(std::memory_order_relaxed);
    return at && GetTickCount64() - at < 500ull &&
           g_padLeftTrigger.load(std::memory_order_relaxed) > XINPUT_GAMEPAD_TRIGGER_THRESHOLD;
}

bool SetFragmentPreUpdate(FragmentPreUpdateFn fn) {   // (v0.9.574)
    g_fragmentPreUpdate.store(fn, std::memory_order_release);
    return g_fragmentHook.load(std::memory_order_acquire);
}

bool HoldClimbTransition(void* agent, const std::uint8_t* transition) {
    if (!g_climbArmed.load(std::memory_order_relaxed) || !agent || agent != g_climbAgent.load(std::memory_order_relaxed) ||
        !transition)
        return false;
    int target = -1;
#if defined(_MSC_VER)
    __try {
#endif
        target = *reinterpret_cast<const std::uint16_t*>(transition + kTransitionTarget);
#if defined(_MSC_VER)
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
#endif
    if (target != g_idxClimb.load(std::memory_order_relaxed)) return false;
    if (!g_climbWheel.load(std::memory_order_relaxed)) return false;  // no wheel: nothing to wait for
    const unsigned need = static_cast<unsigned>(g_climbHoldMs.load());
    const ULONGLONG now = GetTickCount64();
    // up: the game's climb-up at once; held long enough: the wheel at once
    if (!CombatAir::JumpButtonHeld() || CombatAir::JumpButtonHeldMs() >= need) {
        g_climbHoldSince.store(0);
        return false;
    }
    // pressed a moment ago (the press itself starts the climb-up): wait until it is let go or held long enough,
    // never longer than the hold time + 150 ms (the pull-up state goes back to the hang after 12 frames)
    ULONGLONG since = g_climbHoldSince.load();
    if (!since || now - since > 2000) {
        since = now;
        g_climbHoldSince.store(now);
        if (g_climbHoldLogs.fetch_add(1) < 20)
            Log::Detailf("Acro: climb-up waits for the jump button (tap = the game's climb-up, held %u ms = the wheel)", need);
    }
    if (now - since > need + 150) {
        g_climbHoldSince.store(0);
        return false;
    }
    return true;
}

bool IsChainTransition(void* agent, const std::uint8_t* transition) {
    return g_chainArmed.load(std::memory_order_relaxed) && agent && agent == g_chainAgent.load(std::memory_order_relaxed) &&
           transition && (transition == g_chainSc.repeat || transition == g_chainSc.evadeToDive);
}

void DrawPlayerSection() {
    // (v0.9.519: its title is the Enhancements tab's collapsing header)
    if (!g_available) ImGui::BeginDisabled();
    bool enabled = g_enabled.load();
    if (ImGui::Checkbox("Black Widow flips on combat evades", &enabled)) {
        g_enabled.store(enabled);
        SaveSettings();
        g_reapplyRequested.store(true);
    }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
        ImGui::SetTooltip(
            "While aiming, the evades play Black Widow acrobatics (Marvel's Avengers) converted to Lara's skeleton:\n"
            "forward (no enemy in front) a front flip, back a back flip, left / right a side move. Which clip plays\n"
            "for which evade is set in ROTTRFramework\\acro\\evades.ini (\"Reload clips\" reads it again). The clips\n"
            "are added to Lara's animation library as new entries; the change applies from the next evade.");
    }
    bool chain = g_rollChain.load();
    if (ImGui::Checkbox("Black Widow acrobatics chain on the roll", &chain)) {
        g_rollChain.store(chain);
        SaveSettings();
        g_reapplyRequested.store(true);
    }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
        ImGui::SetTooltip(
            "Not aiming, while moving: every roll press plays the next Black Widow move - forward roll, handspring,\n"
            "tucked salto, aerial, running flip - and the chain goes on as long as you keep pressing roll (press it\n"
            "again around each landing). The game's own limit of two moves is lifted.");
    }
    bool noHits = g_noHits.load();
    if (ImGui::Checkbox("Enemy shots miss Lara in rolls and evades", &noHits)) {
        g_noHits.store(noHits);
        SaveSettings();
    }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("While Lara rolls (the roll chain) or evades in combat, and 0.3 s after, enemies keep shooting\n"
                          "but a bullet that would hit her is turned to pass beside her (it hits what is behind): no\n"
                          "blood on the acrobatics. Melee attacks wait (the game's \"incoming melee disabled\" flag).");
    if (noHits) {
        unsigned reads = 0, answered = 0, shots = 0, turned = 0, fxHits = 0, fxPassed = 0, cancelled = 0;
        CombatAir::AcroProtectStats(reads, answered);
        CombatAir::ShotStats(shots, turned);
        CombatAir::FxShotStats(fxHits, fxPassed, cancelled);
        ImGui::Indent();
        ImGui::TextDisabled("Enemy bullets on Lara: %u, %u passed through in acrobatics (+%u of %u instant shots turned); "
                            "shot damage cancelled %u; melee checks %u (%u held off)", fxHits, fxPassed, turned, shots,
                            cancelled, reads, answered);
        ImGui::Unindent();
    }
    bool lbStrafe = g_lbStrafe.load();
    if (ImGui::Checkbox("LB: strafe (heal panel on LB+RB)", &lbStrafe)) {
        g_lbStrafe.store(lbStrafe);
        SaveSettings();
    }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("Hold LB (not aiming): Lara keeps facing ahead and runs / walks forward, back, left and right\n"
                          "with the game's own omnidirectional run cycles, in combat encounters too (the game's in-combat\n"
                          "set of those cycles is empty; it gets the same ones). B while strafing plays the combat evades\n"
                          "(as when aiming). Pressing LB takes her out of cover, and she does not sprint while strafing\n"
                          "(a sprint ends at the press). The game no longer sees LB alone: its heal panel opens with\n"
                          "LB+RB. LT+LB is unchanged (slow motion).");
    if (lbStrafe)
        ImGui::TextDisabled("LB strafe: %s, %u presses; animations %s; in-combat cycles %s",
                            g_padHooks.load() ? (g_strafeHeld.load() ? "held" : "ready") : "no game pad hook",
                            g_strafePresses.load(), g_omniValuesAvailable.load() ? "ready" : "unavailable (movement only)",
                            g_combatOmniBase.load() ? "filled" : g_fragmentHook.load() ? "looking for the graph" : "unavailable");
    if (lbStrafe) {
        ImGui::Indent();
        bool strafeCrouch = g_strafeCrouch.load();
        if (ImGui::Checkbox("Crouched strafe near cover", &strafeCrouch)) {
            g_strafeCrouch.store(strafeCrouch);
            SaveSettings();
            g_reapplyRequested.store(true);
        }
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip("Where the game would crouch Lara behind low cover (or in bushes), the LB strafe stays\n"
                              "crouched for the rest of the hold: crouched versions of the game's run / walk cycles in all\n"
                              "directions (feet stepping the way she moves). Off: she stands up to strafe.\n"
                              "Clips: ROTTRFramework\\acro\\strafe.ini.");
        if (strafeCrouch) ImGui::TextDisabled("Crouched strafe: %s", g_strafeCrouchActive.load() ? "on (near low cover)" : "ready");
        bool strafeTrace = g_strafeTrace.load();
        if (ImGui::Checkbox("Strafe trace in the log", &strafeTrace)) {
            g_strafeTrace.store(strafeTrace);
            SaveSettings();
        }
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip("While LB is held, ROTTRFramework.log gets a line every 0.4 s: Lara's speed, the stick, her\n"
                              "state control states and the animation clips her graph plays (weight, frame, whether the\n"
                              "clip's time moves). For finding why a strafe animation freezes; up to 400 lines a session.");
        ImGui::Unindent();
    }
    bool climbWheel = g_climbWheel.load();
    if (ImGui::Checkbox("Acrobatic climb-up (hold A / Space)", &climbWheel)) {
        g_climbWheel.store(climbWheel);
        SaveSettings();
        g_reapplyRequested.store(true);
    }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
        ImGui::SetTooltip(
            "Hanging from a ledge Lara can climb onto: hold A (Space on the keyboard) and push up - or keep A held\n"
            "from the jump that grabbed the ledge - and she pulls up, presses onto her hands and turns over them onto\n"
            "the platform (the roll chain's second move) instead of the game's climb-up. Without A the climb-up is the\n"
            "game's own. Clips: ROTTRFramework\\acro\\climb.ini.");
    }
    if (climbWheel) {
        ImGui::Indent();
        int holdMs = g_climbHoldMs.load();
        ImGui::SetNextItemWidth(220.0f);
        if (ImGui::SliderInt("Hold A for", &holdMs, 60, 600, "%d ms")) g_climbHoldMs.store(holdMs);
        if (ImGui::IsItemDeactivatedAfterEdit()) SaveSettings();
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("A tap of A on a ledge starts the game's own climb-up; A held at least this long when the\n"
                              "climb-up starts plays the acrobatic one (the climb-up waits that long for the decision).");
        ImGui::TextDisabled("Climb-up: %s", g_climbStatus);
        ImGui::Unindent();
    }
    bool climbVault = g_climbVault.load();
    if (ImGui::Checkbox("Black Widow vault on the climb-up from a ledge", &climbVault)) {
        g_climbVault.store(climbVault);
        SaveSettings();
        g_reapplyRequested.store(true);
    }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("Hanging from a ledge Lara can climb onto: the normal climb-up (without holding A) pulls up into\n"
                          "Black Widow's vault - hands pressed on the edge, a push up, knees tucked, landing on her\n"
                          "feet - instead of the game's climb-up. Holding A still plays the wheel. Clips:\n"
                          "ROTTRFramework\\acro\\climb.ini [ClimbTap].");
    bool bwHurdle = g_bwHurdle.load();
    if (ImGui::Checkbox("Black Widow jump onto high ledges", &bwHurdle)) {
        g_bwHurdle.store(bwHurdle);
        SaveSettings();
        g_reapplyRequested.store(true);
    }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("Climbing onto a high ledge from the ground (the game's hands-on-the-edge climb, ledges of about\n"
                          "1.3 to 1.7 m): Black Widow's jump onto a platform instead - a step on the wall, both hands on\n"
                          "the edge, a tucked vault and the landing on her feet. Clips: ROTTRFramework\\acro\\hurdle.ini.");
    if (bwHurdle) {
        ImGui::Indent();
        ImGui::TextDisabled("High ledges: %s", g_hurdleStatus);
        ImGui::Unindent();
    }
    bool bwJump = g_bwJump.load();
    if (ImGui::Checkbox("Black Widow jump", &bwJump)) {
        g_bwJump.store(bwJump);
        SaveSettings();
        g_reapplyRequested.store(true);
    }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("Lara jumps like Black Widow: the running (and sprint) jump is her leap with the knees tucked\n"
                          "and the arms reaching forward, the standing jump her tucked jump; falls use her legs-down\n"
                          "fall. The height and distance stay the game's; the landings are Lara's.\n"
                          "Clips: ROTTRFramework\\acro\\jump.ini.");
    if (bwJump) {
        ImGui::Indent();
        ImGui::TextDisabled("Black Widow jump: %d animation entries", g_statusPatched.load() ? g_jumpRedirected : 0);
        ImGui::Unindent();
    }
    if (enabled || chain || climbWheel) {
        ImGui::Indent();
        bool sounds = g_flipSounds.load();
        if (ImGui::Checkbox("Sounds on the Black Widow moves", &sounds)) {
            g_flipSounds.store(sounds);
            SaveSettings();
            g_reapplyRequested.store(true);
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip(
                "Lara's own sounds, retimed to the moves: a standing jump's push-off and the evade's whoosh at the\n"
                "takeoff, a jump landing (feet, impact, gear, dust) at the landing; the forward roll uses her roll.");
        if (chain) {
            int distance = g_chainDistance.load();
            ImGui::SetNextItemWidth(220.0f);
            if (ImGui::SliderInt("Chain move distance", &distance, 20, 150, "%d%%")) g_chainDistance.store(distance);
            if (ImGui::IsItemDeactivatedAfterEdit()) {
                SaveSettings();
                g_reloadRequested.store(true);  // the clips are scaled when they are loaded
            }
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("How far each roll chain move carries Lara, in %% of the Black Widow motion and of the\n"
                                  "roll states' own push (the clips are reloaded when you let go of the slider).");
        }
        const int first = g_statusFirstId.load();
        ImGui::TextDisabled("Status: %s", g_statusMessage);
        if (chain) ImGui::TextDisabled("Roll chain: %s", g_chainStatus);
        ImGui::TextDisabled("Clips: %d loaded%s", g_statusClips.load(), first >= 0 ? "" : " (none)");
        if (first >= 0) ImGui::TextDisabled("Animation ids from %d down", first);
        if (g_statusPatched.load()) {
            ImGui::TextDisabled("Lara's library: %d entries + %d new, %d evades redirected", g_statusLibraryCount.load(),
                                g_statusClips.load(), g_statusRedirected.load());
            if (g_flipSounds.load()) {
                int total = 0;
                for (std::size_t i = 0; i < g_defCount; ++i) total += g_statusSounds[i].load();
                ImGui::TextDisabled("Sounds: %d triggers over %zu clips", total, g_defCount);
            }
        }
        if (ImGui::Button("Reload clips"))
            g_reloadRequested.store(true);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Loads ROTTRFramework\\acro\\*.tr10anim again (after converting new versions), no restart needed.");
        ImGui::Unindent();
    }
    if (!g_available) ImGui::EndDisabled();
}
}  // namespace Acro
