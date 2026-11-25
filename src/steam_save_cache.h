#pragma once
// The game's "does this save exist" without asking Steam once per Steam Cloud file (v0.9.339).
//
// SteamSave::SaveExists (ROTTR.exe 0x8A9160, the save system's vtable +0x50) asks ISteamRemoteStorage for the file
// count, then for every file's name (GetFileNameAndSize, a call into the Steam client each) until the save's name comes
// up: N calls per question. The main menu asks for every save it shows (Continue, Load Game), and the 0.9.339 stall
// sampler found the game thread there for 0.5-1.2 s at a time. Now the remote file names are kept and only the file
// count is asked each time: the list is read again when the count changes (a save written or deleted) or when it is
// over 2 s old. (faultrep.dll keeps its own startup copy while the main menu is being built.)
//
// v0.9.473: the main menu's save data read again when it was missed. UiMenuStateLegal::GotoMainMenu calls the save
// system's InitMainMenu (SteamSave::InitMainMenu, its vtable +0x30), which runs SaveLoad::LoadSaveProgressData (retail
// 0x4903A0, TR2 0x4C0FA0): every slot's progress data (SAVE_GetProgressDataForSlot) is cleared and read again only for
// the slots SaveGameExists answers yes to at that moment. The faster loading calls GotoMainMenu on the logo screen's
// first frame, and a save missing from that early answer is never read: the main menu then shows it as an existing save
// with empty data (CONTINUE: "EMPTY", 1/1/1601 - 00:00, 0% complete) and none of the saves is taken as the newest
// (UiMenuStateMainMenu::OnSaveProgressDataUpdated: ReplayInfo::SetSaveSlotToUseForPlayerData), until another menu reads
// them again. Now, 1.5 s after the main menu comes, a save that exists with progress data never read (date 0, not marked
// corrupt) has LoadSaveProgressData run again; when its reads are done SaveLoad::Process calls
// UiMenuStateManager::OnSaveProgressDataUpdated and the main menu takes the data as the game does.
namespace SteamSaveCache {
bool CreateHook();   // MH_CreateHook only; enabled with the other hooks
void OnMainMenuEntered();   // game thread: UiMenuStateMainMenu::OnEnter ran
void TickGameThread();      // game thread, every tick
}
