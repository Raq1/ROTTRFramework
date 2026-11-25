#pragma once
// (v0.9.541) Why the HUD stops answering after some loads (0.9.471, 0.9.481..0.9.486, 0.9.513; again in 0.9.540's run:
// after a Chapter Select load from the main menu the bottom right's loading icon kept running, the weapon keys did
// nothing, TAB opened the map only through the framework's fallback, and the button bar kept being shown with no menu
// up). Logging only, to find what is left over: see hud_watch.cpp.
// (v0.9.587) Also the system message manager (NotificationDisplayManager: while a message is on display every widget's
// input is refused), logged with the gates; a message with no dialog the player can see, 4 s in play, is cleared.
namespace HudWatch {
bool Install();          // its hooks (before the framework enables them)
void TickGameThread();   // the number keys' watch (the game thread)
// The menu stack and every visible widget by class, logged with the reason (the first few calls; the game thread)
void LogCensus(const char* why);
// (v0.9.589) The HUD's weapon select moved to the bottom left at 80% of its size (Enhancements > Gameplay;
// Settings::Get().weaponSelectBottomLeft updated, the caller saves).
bool WeaponSelectLayoutAvailable();
bool WeaponSelectBottomLeft();
void SetWeaponSelectBottomLeft(bool on);
// (v0.9.590) its size there, percent of the game's (30..100; Settings::Get().weaponSelectSize updated)
int WeaponSelectSize();
void SetWeaponSelectSize(int percent);
}  // namespace HudWatch
