#pragma once
#include <windows.h>

// The main menu's EXTRAS option (its last one, Start Benchmark, renamed) opens the Extras menu: the game's Croft Manor menu
// (state 0x93) driven by us (v0.9.359), with CUTSCENE VIEWER, MODEL VIEWER (greyed out, later) and CREDITS (the game's).
// CUTSCENE VIEWER opens the Cutscene Viewer screen: v0.9.497 the game's own Relics list (the pause menu's, state 0x5C: the
// cutscenes of a category as its entries, LB / RB the categories; the picture and the outfit grid on the right), when its
// movie's in-game menu collection can be loaded; else, as before, the game's own Expeditions screen (state 0x82) driven
// by us (v0.9.345).
// Rows: CUTSCENE (the ones Cutscenes::PlayableItems gives, the story's first; LB/RB too), then PLAY; the outfit (the
// story's or any) in a grid on the screen's right (v0.9.496; an OUTFIT row before). The info panel shows the cutscene's
// title, group, description and picture (cutscenes\images\<image>, framed as the game's own pictures and made a 2048x1024 game
// texture shown at 512x256); the header reads EXTRAS / CUTSCENE VIEWER. Back goes to the Extras menu, from there to
// the main menu.
// Playing: picture and sound fade out (0.5 s), the start loads behind black, the cutscene fades in when it starts and out
// 0.5 s before its end, then the main menu, the Extras menu and the viewer screen come back with the same choices.
namespace CutsceneViewer {
bool CreateHooks();                       // the main menu's hooks (before MinHook's queued enable)
void AddFonts();                          // with the overlay's fonts (EnsureImGuiPlatform)
void TickGameThread();                    // playback: fades, sound, outfit, the way back to the menu
bool NeedsFrame();                        // the overlay draws a frame for the viewer or a fade
bool FullBlack();                         // the screen is all black (the overlay clears it: every frame black)
void Draw();                              // inside the overlay's ImGui frame
bool WantsInput();
bool InUse();                             // (v0.9.602) a cutscene of the viewer playing, or the Model Viewer open                        // the viewer is open: it takes mouse and keyboard (and the pad)
// The game's window message kept from it: while a cutscene plays from the viewer, its input; on the Model Viewer's view,
// its input but the mouse over the view's button bar.
bool BlocksGameMessage(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
bool HandleKey(UINT msg, WPARAM wParam);  // the window's keys: Esc leaves a playing cutscene
bool FilterPad(unsigned long user, void* xinputState);  // XInputGetState's result: read, and emptied for the game
void Open();                              // the framework menu's button: the Extras menu (from the main menu)
const char* MenuStatus();
}  // namespace CutsceneViewer
