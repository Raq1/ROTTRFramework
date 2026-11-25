#pragma once
#include <cstdint>

namespace Acro {
bool Initialize();
void TickGameThread();
void DrawPlayerSection();
// Called by the SCAgent::DoTransition hook (combat_air.cpp) for every agent's transition, before the game's own
// processing: drives the roll chain (which Black Widow clip the next roll state plays).
void OnStateTransition(void* agent, const std::uint8_t* transition);
// Called by the SCAgent::CheckConditions hook for every transition that passed its conditions: false holds the roll
// chain's next move until the current move has landed (its clip's "ready" frame).
bool AllowTransition(void* agent, const std::uint8_t* transition);
// True for the roll chain's own transitions (ground_evade_to_evade_dive / ..._dive_repeat) while the chain is on.
bool IsChainTransition(void* agent, const std::uint8_t* transition);
// True while Lara's primary agent plays a roll chain move (ground_evade / ground_evade_dive with the chain on).
bool IsChainMove(void* agent);
// True while a roll chain move plays and has reached its clip's ready frame (frames in the state and that frame are
// returned); unlike AllowTransition false outside a chain move.
bool ChainMoveReady(void* agent, float* framesOut, int* readyOut);
// Called for every transition that passed its conditions: true holds back Lara's entry into the pull-up state while the
// jump button is down but not yet held for the climb-up's hold time (a tap -> the game's climb-up, a hold -> the wheel).
bool HoldClimbTransition(void* agent, const std::uint8_t* transition);
// True when enemies should not hit Lara during rolls and combat evades (setting "NoHitsInAcrobatics").
bool AcrobaticsProtection();
// True while LB is held for the strafe (setting on, the game's pad read seen): B then plays the combat evades.
bool StrafeHeld();
// Called for every transition that passed its conditions: true holds back Lara's way into a sprint state while the LB
// strafe is held (the sprint has forward-only cycles): any of her agents, the sprint being the Sub-State Modifiers
// agent's ground_sprint.
bool HoldBackWhileStrafing(void* agent, const std::uint8_t* transition);
// (v0.9.574) A function the AnimFragmentNode::Update hook (retail 0x1381B0, this module's) calls for every fragment
// node before the game's Update, on whatever thread the graph updates (dual wield: the shot's node of the pistol that
// did not fire, ended before it moves). MinHook takes one hook per function, so other modules go through this one.
// Null: none. False: the hook is not installed (its bytes differ), the function is never called.
using FragmentPreUpdateFn = void (*)(void* node, void* animData);
bool SetFragmentPreUpdate(FragmentPreUpdateFn fn);
// (v0.9.582) True while the game's last pad read (the XInputGetState hooks, within 500 ms) had the left trigger (the aim)
// past XInput's threshold. Any thread.
bool PadAimHeld();
}
