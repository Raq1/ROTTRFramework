#pragma once

// The text and voice languages switched in game (v0.9.473).
//
// Options > Audio (UiMenuStateNixxesOptionsScreenAudio::OnListInitialized, retail 0x922050, TR2 0x965DC0) builds its
// VOICE LANGUAGE and TEXT LANGUAGE steppers enabled only when the game state is 2 (MENUING, the main menu; 5 is
// PLAYING): "cmp eax, 2; ...; sete bl" (retail 0x922095), bl stored as both items' enabled flag (ItemData +0x38, passed
// to the movie's addOptionStepper). That sete becomes "mov bl, 1; nop", so the steppers can be changed in a level too.
// (v0.9.473 wrote "xor bl, bl", taking the flag for a disabled one: the languages were greyed out everywhere.)
// The rest is the game's own way, as at the main menu: Back asks OptionsMenu_LanguagesWarning, a yes goes to UiMenuStateLanguageReload (MENU_STATE 0xBE), whose OnEnter runs
// PlayerProfile::ApplyLanguageOptions (localstr_change_language: the text strings and the file system's masks), then
// back to the options. Text read after that is in the new language; voice streams opened after it (dialogue) play in
// the new voice language; what the level has already loaded (sound banks, a cutscene's lines) stays as it was until it
// is loaded again (a checkpoint or the next level).
namespace LanguageOptions {
bool Install();   // the byte patch, after the game's code is checked
bool Installed();
}
