#pragma once
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

// Custom campsite outfits (v0.9.310; new models v0.9.311).
//
// Each ROTTRFramework\outfits\<Name>\outfit.ini (or ROTTRFramework\outfits\<Name>.ini) adds one campsite outfit:
//   [Outfit]
//   Base=laracroft_classic05        object of the retail outfit this one is copied from (unlock, recipe, sort
//                                   order, menu image, character profile)
//   Object=laracroft_classic01      object the outfit wears: an object the game has, or (v0.9.311) a new name when
//                                   the outfit's folder holds its resource files (OutfitArchive builds its .drm)
//   Model=laracroft_classic05       (new models) the outfit whose .drm the new one is built from (default: the
//                                   outfit whose .drm has the supplied resource IDs, else Base)
//   Sources=laracroft_witch,...     (new models) more outfits to take referenced resources from
//   Files=..\..\MyModel             (new models) folder of the resource files, if not the outfit's own folder
//   Name=My Outfit                  campsite name (optional; the base outfit's otherwise)
//   Description=...                 campsite description (optional)
//   Author=... / Version=...        shown for packages (optional)
//   [Toggles]                       (v0.9.331) checkboxes in the Outfits menu while the outfit is worn:
//   Glasses=178450,178451           material IDs hidden together; ",off" = hidden by default
//   (mods: mod.json toggles, "title" being the name shown in the game; v0.9.450 "not_with": the toggles one is never
//   shown with: v0.9.454 showing it hides those that are shown; v0.9.451 an outfit's "gear": false (v0.9.455
//   "hide_lower_gear": true; and "hide_upper_gear": true, "hide_markers": [IDs]): Lara's gear on those markers is not
//   drawn while it is worn, Features::SetPlayerGearHiddenGameThread. The campsite's Outfits screen lists the
//   highlighted outfit's toggles in its panel's COST / INVENTORY table renamed ITEM / STATE (six rows past three):
//   Y on a game pad / [Outfits] OutfitsScreenPartsKey (default T) on the keyboard edits them, a click toggles one;
//   [Outfits] OutfitsScreenParts=0 leaves that screen as is. The Inventory's third tile, "Custom Parts", lists the
//   worn outfit's: [Outfits] CustomPartsMenu=0 leaves the Inventory as is)
//
// Packages (v0.9.331): ROTTRFramework\outfits\<Name>.zip holds an outfit folder (outfit.ini at the top of the .zip or
// in one folder). It is unpacked to outfits\_packages\<Name>\ (again only when the .zip changes) and used like a
// folder. *.lua files next to an outfit.ini (folders and packages) are loaded with the other scripts.
//
// Mods (v0.9.332): <game>\ROTTRFramework\mods\<Mod>\mod.json (or <Mod>.zip there, unpacked to its _cache\; before
// v0.9.605 <game>\mods\, still read after it for the names it lacks), written by the
// ROTTR Outfit Manager. "outfits": new slots (object, base, name, description, optional model folder, toggles); a
// slot without a model is a copy of its base under the new object name. "overrides": changes to retail outfits by
// their debug name (name, description, "unlock": "always"). A folder starting with "_" is disabled.
//
// The game's outfit list (dtp::GlobalCrafting::m_outfitsList, 0xA8 per outfit) and character profile list
// (dtp::GlobalUIInfo +0x10, 0xA8 per profile, count byte at +0x18) are replaced by copies with the custom entries
// appended, on the game thread, whenever the game (re)loads them. Text: localstr_get answers the custom string IDs.
// Custom outfits are always unlocked (v0.9.330; before, they followed the base outfit). The campsite's "last outfit worn" (saved in the profile) stores the
// base outfit for a custom one, so a profile stays valid without the framework; the custom choice is kept in
// ROTTRFramework\outfits\_state.ini.
namespace CustomOutfits {
// Right after MH_Initialize: reads the configs and, when outfits ship new models, builds the framework archive on a
// thread (OutfitArchive). The game's archive scan (FindFirstFileExA "*.nfo") waits for it.
void StartEarly();
// Reads the configs and creates the hooks (enabled with the other hooks). Before MH_ALL_HOOKS is enabled.
bool Install();
// Game thread, once per tick: applies the tables when the game data is there (cheap when nothing changed).
void TickGameThread();

int ConfiguredCount();
int ActiveCount();            // entries in the game's tables right now
const char* Status();
// CustomImageLoader::LoadImage, shared: MinHook makes one hook a function, and this module makes it when custom outfits
// are configured (Install). Another module's look at each image loaded (the Cutscene Viewer's pictures: their size in
// the movie), called after this module's own, on the thread that loads it.
using ImageLoadedFn = void (*)(const char* url, void* image);
bool HooksImageLoad();                        // after Install: the hook is this module's
void SetImageLoadedListener(ImageLoadedFn listener);
// Outfit-list index range of the custom outfits (first, count), or false while not applied.
bool CustomIndexRange(unsigned int& first, unsigned int& count);
// New-model outfits: the new IDs given to a supplied material whose ID the game uses elsewhere (a script hiding
// material <id> reaches the outfit's copy through them). Empty when the material kept its ID. Any thread.
int RenamedMaterialIds(std::uint32_t id, std::uint32_t* out, int capacity);
// Every ID a resource of the game (type, id) has in the built outfits: the mods' renamed copies of it and the game's
// resources copied into the outfits under new IDs (their object data...). For the Modding Suite's live edits.
int ResourceCopies(std::uint8_t type, std::uint32_t id, std::uint32_t* out, int capacity);

// [Toggles] of the custom outfit the player wears (none: WornOutfitName() is nullptr). Any thread.
struct ToggleInfo {
    const char* name = nullptr;
    bool on = true;
    std::string hides;   // (v0.9.454) hidden: the shown toggles showing it hides (never shown with), ", " between
};
const char* WornOutfitName();
int WornToggleCount();
bool GetWornToggle(int index, ToggleInfo& out);
// Saved in outfits\_state.ini, applied on the next game tick. False: no worn outfit or no such toggle. (v0.9.454)
// Turned on, the shown toggles it is never shown with are turned off (saved too): their indices in `hidden`.
bool SetWornToggle(int index, bool on, std::vector<int>* hidden = nullptr);

// Folders of outfit folders and packages, for their *.lua scripts (after StartEarly).
struct ScriptFolder {
    std::string outfit;
    std::filesystem::path folder;
};
std::vector<ScriptFolder> ScriptFolders();
}
