#pragma once

// Free underwater depth control and breath tuning for the local player.
namespace SwimBreath {
// Verifies the retail code and creates (not enables) the MinHook hooks. Call before MH_ApplyQueued.
bool CreateHooks();
// "Swimming" section of the Player tab.
void DrawPlayerSection();
// (v0.9.586) Lara under water right now (in a water volume and holding her breath, as the swim code last saw her;
// false once its tick is more than half a second old). Any thread (LaraFM's muffle).
bool LaraUnderwater();
}
