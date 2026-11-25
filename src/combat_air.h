#pragma once

// Aiming while Lara is in the air, and a slow-motion button.
namespace CombatAir {
// Verifies the retail code and creates (not enables) the MinHook hooks. Call before MH_ApplyQueued.
bool CreateHooks();
// Once per game tick, before the game's own processing (Gameloop::DoTimeProcess hook).
void TickGameThread();
// "Air aim & slow motion" section of the Player tab.
void DrawPlayerSection();
// Lara's primary state control agent (SCAgent*), or nullptr (no player yet / lookup unavailable).
void* LaraPrimaryAgent();
// True when the SCAgent::DoTransition hook is installed (it forwards every transition to Acro::OnStateTransition).
bool TransitionHookActive();
// True while the jump button is held: the pad's A (XInput) or Space with the game window in front. Any thread.
bool JumpButtonHeld();
// How long the jump button has been held without a break (ms, from the per-frame tick; 0 when up or just pressed).
unsigned JumpButtonHeldMs();
// Acrobatics protection counters: reads of Lara's IncomingAttack_*Disabled and how many were answered true.
void AcroProtectStats(unsigned& reads, unsigned& answered);
// Enemy shots seen by the PROJECTILE_FireInstant hook and how many were turned away from Lara in acrobatics.
void ShotStats(unsigned& shots, unsigned& turned);
// FX projectile (enemy bullet) collisions with Lara, how many passed through her in acrobatics, and shot damage cancelled.
void FxShotStats(unsigned& hits, unsigned& passed, unsigned& cancelled);
// Game thread: the camera's aim direction (world, unit) for the player's instance; false when it cannot be read.
bool CameraDirection(void* instance, float dir[3]);
// True while the aim input (LT / right mouse) is held with the air aim on (from the per-frame tick). Any thread.
bool AimHeld();
}
