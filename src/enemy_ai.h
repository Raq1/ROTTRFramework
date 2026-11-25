#pragma once

// Enemy behaviour for close combat: fire tokens, so that the enemies do not all shoot Lara at once.
namespace EnemyAI {
// Verifies the retail code, reads the settings and installs the hooks. Call after MH_Initialize.
bool Initialize();
// Once per game tick, before the game's own processing (Gameloop::DoTimeProcess hook): the periodic log summary.
void TickGameThread();
// "Enemy behaviour" section of the Player tab.
void DrawPlayerSection();
}
