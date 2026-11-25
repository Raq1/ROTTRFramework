#pragma once
#include <cstdint>

// Hand-to-hand combat: X next to an enemy throws Black Widow's punches and kicks, one after the other for as long as
// the button is held or pressed again. The game's melee push states carry the clips; the pace of the chain, the
// lock-on, the approach, the hit and the sounds are ours (see brawl.cpp).
namespace Brawl {
using DoTransitionFn = void(__fastcall*)(void* agent, const std::uint8_t* transition);

bool Initialize();
// Once per game tick, before the game's own processing (Gameloop::DoTimeProcess hook).
void TickGameThread();
// "Hand-to-hand" section of the Player tab.
void DrawPlayerSection();
// The game's pad read (acro.cpp's XInputGetState hooks), after the strafe's remap: X with an enemy in reach is taken
// from the game (each press is one strike); the game's melee button (Y) is pressed for it only when a first strike
// cannot be forced. Any thread.
void RemapPadButtons(std::uint16_t& buttons);
// SCAgent::DoTransition hook (combat_air.cpp), before the game's: Lara entering a melee push state after the brawl's
// melee press starts a strike; leaving the melee states ends it.
void OnStateTransition(void* agent, const std::uint8_t* transition);
// SCAgent::CheckConditions hook, for a transition that passed: false holds the game's own melee follow-ups (turn,
// step 2, "hit the enemy", finishers, seizes) while a brawl strike plays.
bool AllowTransition(void* agent, const std::uint8_t* transition);
// SCAgent::CheckConditions hook, for a transition that did not pass: true lets one of Lara's melee transitions through
// when the next strike of the chain is due (RedirectTransition then sends it to the chain's next state).
bool ForceTransition(void* agent, const std::uint8_t* transition);
// SCAgent::DoTransition hook: the transition ForceTransition let through runs as a change to the chain's next melee
// state (true: done, the game's own transition is not run).
bool RedirectTransition(void* agent, const std::uint8_t* transition, DoTransitionFn original);
// The dual wield's fragment node hook (features.cpp), with the state graph data (dtp::AnimStateGraph) of a pistol's
// hosted graph: the one with the melee states gets its edges into and between them found (their blend frames). Any thread.
void OnPistolGraph(const std::uint8_t* graph);
// True while a brawl strike plays or one started in the last 4 s (the enemies' melee tokens follow it). Any thread.
bool ComboRunning();
// True while Lara fights hand to hand: a brawl strike plays, is asked for or started in the last 2.5 s, or the game's
// own melee (the axe, a finisher) ran in the last 1.5 s. The enemies' fire tokens hold back then. Any thread.
bool InMelee();
// The enemy the brawl is locked on to while InMelee() (its Instance*, only to compare), or nullptr. Any thread.
const void* LockedEnemy();
// PlayerSteering::Process hook: while a strike runs Lara faces the locked enemy (unit x, y; degrees per second).
bool FacingOverride(float& dirX, float& dirY, float& rateDegPerSec);
// The camera's distance factor of the dodge camera: 1 outside a dodge, up to 1 + DodgeCameraExtra while Lara is in a
// combat evade or a roll, eased in and out. Read by the aim camera hook (combat_air.cpp); the exploration camera is
// hooked here. Any thread.
float DodgeCameraFactor();
// True for 700 ms after the brawl itself pressed the game's melee button (Y) for a strike: that press is not the
// player's. Any thread.
bool PressedMeleeRecently();
// True while the melee press belongs to the brawl: hand-to-hand on and a living enemy inside its range, or a brawl
// strike just asked for. The grapple kick leaves the press alone then. Any thread.
bool ClaimsMeleePress();
}
