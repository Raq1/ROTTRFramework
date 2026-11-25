#pragma once
#include <cstdint>

// Black Widow style grapple kick: the melee button with an enemy at range (combat stance, on foot, or in a jump) zips
// Lara to the enemy and ends in a kick (the cut combat_melee_kick state of her state control).
namespace Grapple {
using DoTransitionFn = void(__fastcall*)(void* agent, const std::uint8_t* transition);

bool Initialize();
// Once per game tick, before the game's own processing (Gameloop::DoTimeProcess hook).
void TickGameThread();
// "Grapple kick" section of the Player tab.
void DrawPlayerSection();
// SCAgent::DoTransition hook (combat_air.cpp), after Acro::OnStateTransition: Lara's melee press in the combat stance
// (or the transition ForceTransition let through in a jump) goes to combat_melee_kick instead while a target is in
// range; the original then runs with the melee transition's target switched for the call. Returns true when it
// handled the call itself (the hook must not run the original again).
bool RedirectTransition(void* agent, const std::uint8_t* transition, DoTransitionFn original);
// SCAgent::CheckConditions hook, for a transition that failed: true lets it through once so that a grapple kick
// armed in a jump (melee pressed while aiming, target in range) can start; RedirectTransition turns it into the kick.
bool ForceTransition(void* agent, const std::uint8_t* transition);
// SCAgent::CheckConditions hook, for a transition that passed: false holds combat_melee_kick's own exit until the
// grapple kick clip has played.
bool AllowTransition(void* agent, const std::uint8_t* transition);
// PlayerSteering::Process hook (Lara's steering only): while a grapple kick turns towards its target, the direction
// to face (unit x, y) and the turn rate in degrees per second.
bool FacingOverride(float& dirX, float& dirY, float& rateDegPerSec);
// AnimBuildTransforms hook (combat_air.cpp), Lara only, after the game's: her bone matrices (16 floats each, the world
// transform with the translation in the last row); the grapple rope leaves from the clip's rope hand. Any thread.
void OnLaraTransforms(std::uint32_t count, const float* out);
// Overlay (render thread, inside the ImGui frame): true while there is a target marker or a rope to draw, and the
// drawing itself (ImGui's background draw list, projected with the draw tools' camera snapshot).
bool OverlayActive();
void DrawOverlay();
}
