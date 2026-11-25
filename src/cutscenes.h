#pragma once
#include <map>
#include <string>
#include <vector>
// The cutscene catalog and playback (v0.9.341: the catalog and a test panel; v0.9.342: the main menu's Extra > Cutscene
// Viewer, cutscene_viewer.cpp).
//
// The game's real-time cutscenes are multiplex streams, pcx64-w\cinstream\<name>.mul (the Suite's cine.cpp reads
// them). Each plays in a unit (level): the unit's dtp::UnitData lists its cutscenes (m_numCinematics +0x68,
// m_CinematicList +0x78, names), and the game's scripts play one by its index in that list and the unit's stream ID:
// NtCinematic::SetCine(index, unitId) -> _NtCineImpl::Get(unitId, index), _NtCineImpl::Play(create) (EVENT_
// CreateCinematic(name, unitId) + EVENT_PlayCinematic), IsPlaying, NtCinematic::Stop (TR2 PDB; retail RVAs in
// cutscenes.cpp). Actor -1 of a cutscene is the player, so Lara plays it as she is dressed.
//
// The catalog: <game>\ROTTRFramework\cutscenes\cutscenes.json, written at start with every cutscene found in the
// game's archives (as the Suite's Cine::FindNames: names in ROTTR.exe and the object names whose stream exists, then
// every unit's list, until no new one) and the pre-rendered videos (movies\pc\*.bik). Each entry: id, title,
// category (Story, Baba Yaga, Blood Ties, Cold Darkness: from the archive holding it), description, titles and
// descriptions (in the game's other text languages), image (cutscenes\images\<id>.jpg), hidden, and the unit
// (refreshed each start). Titles, categories, descriptions and images are the user's to edit: entries found again keep
// what the file says; new ones are added.
namespace Cutscenes {
void Initialize();       // starts the catalog scan (a background thread)
bool CreateHooks();      // the level scripts' cutscene nodes (Cine_PlayCinematic), before MinHook's queued enable
void TickGameThread();   // game thread: the test panel's requests, a playing cutscene's end
void DrawTestPanel();    // the framework menu's Cutscenes tab

// For the Cutscene Viewer (cutscene_viewer.cpp).
struct ViewerItem {
    std::string id, title, category, description, image;
    std::map<std::string, std::string> titles, descriptions;   // by the game's language code (cutscenes.json)
};
bool CatalogReady();
void ReloadUserText();                       // titles, categories, descriptions, images from cutscenes.json again
std::wstring ImagesDirectory();
std::vector<ViewerItem> PlayableItems();     // not hidden, real-time, with a start in the game's Chapter Select list;
                                             // in the order the game plays them (its Chapter Select story order)
bool PlayForViewer(const std::string& id);   // loads its start; when it ends, back to the main menu
enum class Stage { Idle, Loading, PreRoll, Playing };   // PreRoll: loaded, ours not playing yet (also when started
Stage CurrentStage();                                   // directly, until its stream plays; and between two cutscenes
                                                        // the story plays one after the other, after 0.25 s)
bool AnyCutscenePlaying();                   // game thread
float TargetSecondsLeft();                   // of the chosen cutscene while it plays (-1: not known)
bool LastViewerPlayFailed();                 // it did not come after the load (back to the menu)
std::string ViewerTrigger(const std::string& id);   // "node", "load" or "" (not played yet)
struct PictureLayout {                       // cutscenes.json "pictures": the pictures in the viewer's info panel
    int width = 512, height = 256;           //   their size (the panel's units; the game's own are 512x256)
    int x = 0, y = 0;                        //   moved by
    int textureWidth = 2048, textureHeight = 1024;   // made at (the file cropped to its shape and scaled; 4s)
    bool frame = true;                       //   files without alpha get cutscenes\frame.png's edges
};
PictureLayout CurrentPictureLayout();
void HoldNodeFiring(bool hold);              // the level node waits (the viewer swaps Lara's outfit first)
void EndViewerMode();
void AbortToMainMenu();
void GameStates(int& game, int& net);        // the game's GameStateID and NetStateID now (-1: not known); game thread
struct UnitSlot {                            // a StreamTracker slot (StreamUnit): its stream ID, state (0 free, 1 being
    int id = 0;                              //   loaded, 2 loaded, 3 not wanted, 4 dumping) and name
    int state = 0;
    char name[48] = {};
};
int StreamUnitSlots(UnitSlot* out, int capacity);   // the 32 slots, in order
// (v0.9.520) The time of day's own stream layers switched with it (game thread; the WeatherManager's type indices).
// (v0.9.521) the loaded units' time-of-day layers made those of a time of day (the game's own for it, else the
// game's mapped to it); game thread, nothing while the game loads
void SwitchTimeOfDayLayers(int toType, const char* reason);
// (v0.9.521) once per load, 2 s after it: the time-of-day layers to be matched again (game thread)
bool TimeOfDayLayersDue();
}  // namespace Cutscenes
