#include "custom_outfits.h"
#include "game_build.h"
#include "log.h"
#include "settings.h"
#include "zone_list.h"
#include "outfit_archive.h"
#include "features.h"
#include "zip_reader.h"
#include "sound_replace.h"
#include "subtitle_edit.h"
#include "outfit_menu_rows.h"
#include "voice_edit.h"
#include "weapon_mods.h"

#include "nlohmann/json.hpp"

#include <Windows.h>
#include <MinHook.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <map>
#include <memory>
#include <mutex>
#include <deque>
#include <string>
#include <thread>
#include <vector>

namespace fs = std::filesystem;

namespace CustomOutfits {
namespace {
// Retail (Steam ROTTR.exe 0x632D2D99) data and functions.
constexpr std::uintptr_t kRvaGlobalCraftingPtr = 0x02D57430ull;   // dtp::GlobalCrafting*
constexpr std::uintptr_t kRvaGlobalUIInfoPtr = 0x02E1E720ull;     // dtp::GlobalUIInfo*
constexpr std::uintptr_t kRvaLocalstrGet = 0x000832D0ull;          // const char* localstr_get(int id, bool* found)
constexpr std::uintptr_t kRvaSetLastOutfitWorn = 0x00867350ull;    // PlayerProfile::SetLastOutfitWorn(int)
constexpr std::uintptr_t kRvaIsOutfitUnlocked = 0x00862FC0ull;     // PlayerProfile::IsOutfitUnlocked(unsigned)
constexpr std::uintptr_t kRvaHasOutfit = 0x00453010ull;            // Crafting::HasOutfit(Instance*, unsigned)
constexpr std::uintptr_t kRvaHasDlcForOutfit = 0x00452F80ull;      // Crafting::HasDLCForOutfit(unsigned)
// CustomImageLoader::LoadImage(const char* url): Scaleform's "img://#<id>" images (the campsite's outfit pictures) from
// the game's textures: the texture by its ID, a GImageInfo of its width x height (+0x30, +0x34). Hooked to log what the
// pictures load as (a picture of another size than the game's showed nothing: found or not, and its size).
constexpr std::uintptr_t kRvaCustomImageLoad = 0x009EC5F0ull;
constexpr std::uint8_t kCustomImageLoadSig[] = {0x40, 0x53, 0x55, 0x48, 0x83, 0xEC, 0x28, 0x48, 0x8B, 0xDA, 0x48, 0x8B, 0xE9, 0x48, 0x8B, 0xCB};
// UiMenuStateCampsiteGear::IsOutfitCraftable(const dtp::OutfitsData*): the campsite menu (SortOutfits) lists an
// outfit when Crafting::HasOutfit says the player has it, else when this says it can be crafted; neither: not listed.
constexpr std::uintptr_t kRvaIsOutfitCraftable = 0x008FD810ull;
constexpr std::uint8_t kIsOutfitCraftableSig[] = {0x48, 0x89, 0x5C, 0x24, 0x08, 0x57, 0x48, 0x81, 0xEC, 0x20, 0x01, 0x00, 0x00};
// cdc::ObjectZoneManager singleton: +0x140 highest zone ID, +0x148 names (zoneId - 1), +0x158 name -> ID map.
constexpr std::uintptr_t kRvaObjectZoneManagerPtr = 0x01A50B20ull;
constexpr std::uintptr_t kRvaZoneNameMapInsert = 0x002256C0ull;     // HashMap<HashString, unsigned>::operator[]
constexpr std::uintptr_t kRvaClearZoneNames = 0x0022F1E0ull;        // ObjectZoneManager::ClearZoneNames

constexpr std::uint8_t kHasOutfitSig[] = {0x40, 0x53, 0x48, 0x83, 0xEC, 0x20, 0x83, 0xFA, 0xFF, 0x74, 0x5C, 0x8B,
                                         0xC2, 0x48, 0x69, 0xD8, 0xA8, 0x00, 0x00, 0x00, 0x48, 0x8B, 0x05};
constexpr std::uint8_t kHasDlcForOutfitSig[] = {0x40, 0x53, 0x48, 0x83, 0xEC, 0x20, 0x8B, 0xC1, 0x48, 0x69, 0xD8, 0xA8,
                                               0x00, 0x00, 0x00, 0x48, 0x8B, 0x05};
constexpr std::uint8_t kLocalstrGetSig[] = {0x48, 0x85, 0xD2, 0x74, 0x03, 0xC6, 0x02, 0x01, 0x83, 0xF9, 0xFF, 0x75, 0x10};
constexpr std::uint8_t kSetLastOutfitWornSig[] = {0x48, 0x89, 0x5C, 0x24, 0x08, 0x57, 0x48, 0x83, 0xEC, 0x20, 0x48,
                                                  0x83, 0x79, 0x38, 0x00, 0x8B, 0xFA, 0x48, 0x8B, 0xD9, 0x74, 0x1A};
constexpr std::uint8_t kZoneNameMapInsertSig[] = {0x48, 0x89, 0x5C, 0x24, 0x10, 0x57, 0x48, 0x83, 0xEC, 0x30, 0x48,
                                                  0x8B, 0xDA, 0x4C, 0x8B, 0xC2, 0x48, 0x8D, 0x54, 0x24, 0x20, 0x48,
                                                  0x8B, 0xF9, 0xE8};
constexpr std::uint8_t kClearZoneNamesSig[] = {0x40, 0x53, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x8B, 0xD9, 0x48, 0x81,
                                               0xC1, 0x58, 0x01, 0x00, 0x00, 0xE8};
constexpr std::uint8_t kIsOutfitUnlockedSig[] = {0x4C, 0x8B, 0x41, 0x38, 0x4D, 0x85, 0xC0, 0x74, 0x1E, 0x8B, 0xC2,
                                                 0x83, 0xE0, 0x3F, 0x0F, 0xB6, 0xC8, 0x8B, 0xC2, 0x48, 0xC1, 0xE8,
                                                 0x06, 0x49, 0x8B, 0x84, 0xC0, 0x68, 0x46, 0x00, 0x00};

// dtp::GlobalCrafting: +0x30 m_numOutfits, +0x38 m_outfitsList. dtp::OutfitsData (0xA8): +0 m_name (string ID),
// +2 m_description, +4 m_playerObject (zone ID), +0x80 m_debugName.
constexpr std::ptrdiff_t kCraftingNumOutfits = 0x30;
constexpr std::ptrdiff_t kCraftingOutfitsList = 0x38;
constexpr std::size_t kOutfitStride = 0xA8;
constexpr std::ptrdiff_t kOutfitName = 0x00;
constexpr std::ptrdiff_t kOutfitDescription = 0x02;
constexpr std::ptrdiff_t kOutfitPlayerObject = 0x04;
constexpr std::ptrdiff_t kOutfitDebugName = 0x80;
// +0x34 m_sortOrder (u16): the campsite menu's order (UiMenuStateCampsiteGear::SortOutfits sorts its owned and its
// craftable outfits by it with qsort: CompareOutfitSortOrder; the retail list has 0 everywhere, so the list's order
// shows).
constexpr std::ptrdiff_t kOutfitSortOrder = 0x34;
// +0x38 m_previewImage (dtp::ImagePath): +0 m_imageRes (the image's DTP), +8 m_filepath (its .drm under
// pcx64-w\design\image resources\).
constexpr std::ptrdiff_t kOutfitImageRes = 0x38;
constexpr std::ptrdiff_t kOutfitImagePath = 0x40;
// dtp::GlobalUIInfo: +0x10 character profiles, +0x18 their count (a byte; the game indexes them by a signed char, so
// 127 at most; with the profile patch below by an unsigned one, 0xFF meaning none: 255).
// dtp::CharacterProfileItem (0xA8): +0 m_debugName, +0x38 m_objectID, +0x40 m_objectIDName, +0x4C m_menuObjectID,
// +0x50 m_menuObjectIDName.
constexpr std::ptrdiff_t kUiProfiles = 0x10;
constexpr std::ptrdiff_t kUiProfileCount = 0x18;
constexpr std::size_t kProfileStride = 0xA8;
constexpr std::ptrdiff_t kProfileDebugName = 0x00;
constexpr std::ptrdiff_t kProfileObjectId = 0x38;
constexpr std::ptrdiff_t kProfileObjectName = 0x40;
constexpr std::ptrdiff_t kProfileMenuObjectId = 0x4C;
constexpr std::ptrdiff_t kProfileMenuObjectName = 0x50;
unsigned int g_maxProfiles = 127;   // 255 once the profile patch is in place (InstallProfilePatch)
// The outfit list's entries at most: the campsite's Outfits list shows the ones worn or craftable, then the broken
// outfits' parts, in OutfitMenuRows::Capacity() rows (64 kept for the parts). Without that patch, none of ours.
unsigned int MaxOutfits() {
    const unsigned int rows = OutfitMenuRows::Capacity();
    return rows > 64 ? std::min(rows - 64, 1024u) : 0;
}

// Custom strings take IDs from the top of the (signed 16-bit) string ID range, two per outfit.
constexpr int kStringIdTop = 0x7FFF;

struct OutfitConfig {
    std::string key;           // folder or file name
    std::string baseName;
    std::string objectName;
    std::string name;
    std::string description;
    std::string model;                 // Model=: the outfit whose .drm a new model is built from (default: Base)
    std::vector<std::string> sources;  // Sources=: more outfits to take referenced resources from
    fs::path folder;                   // the outfit's folder (its resource files), empty for single .ini files
    bool buildsModel = false;          // the folder has resource files: the framework builds Object's .drm
    std::string author;                // Author= / Version=: shown in the Outfits menu (optional)
    std::string version;
    fs::path packageFolder;            // the folder holding outfit.ini (folders and .zip packages): its *.lua run
    bool package = false;              // from a .zip (outfits folder or mods folder)
    bool cloneOnly = false;            // mods: no model files, a copy of the base outfit under the new object name
    std::vector<std::string> exclude;  // mods: model files to leave out ("exclude": relative, lower case, '/')
    std::vector<fs::path> shared;      // mods: the mod's shared files it uses ("shared"), as if in its folder
    fs::path picture;                  // mods: the campsite menu picture (.dds, built into the framework archive)
    std::string mod;                   // mods: the mod (folder or .zip name) the outfit comes from
    std::string replaces;              // mods: a retail outfit (m_debugName) this one replaces in the campsite list
                                       // (its model, picture, toggles) instead of being added after the game's
    struct Toggle {
        std::string name;
        std::vector<Features::HiddenModelPart> parts;   // mesh parts of the outfit's models (v0.9.398: no longer its
                                                        // materials: a part alone, not every part with its material)
        bool on = true;                    // shown (guarded by g_toggleMutex)
        std::string title;                 // mods: the name shown in the game (empty: the name)
        std::vector<std::string> notWith;  // mods (v0.9.450): the toggles it is never shown with ("not_with": names)
        // mods (v0.9.602, v0.9.604 several): "holstered": the weapon types ("bow", "pistol", "rifle", "shotgun", one
        // or a list) its parts follow: a bit per Features::WeaponClass (0: none, a toggle of the menus). Drawn only
        // while Lara has one of them holstered (Features::HolsteredWeaponClasses), or with "holster_hides": true
        // (v0.9.604) hidden while she has; not in the menus (always on: the holster alone shows or hides it)
        unsigned holstered = 0;
        bool holsterHides = false;
    };
    std::vector<Toggle> toggles;
    std::vector<std::size_t> menuToggles;   // (v0.9.604) the toggles the menus list (not following a holster), in order
    bool hideLowerGear = false;        // mods (v0.9.451): "hide_lower_gear": true (before v0.9.455 "gear": false): Lara's
                                       // lower gear not drawn while it is worn (Features::SetPlayerGearHiddenGameThread)
    bool hideUpperGear = false;        // mods (v0.9.455): "hide_upper_gear": true: the weapons on her back not drawn
    std::vector<std::int32_t> hideMarkers;   // mods (v0.9.455): "hide_markers": [IDs]: what is put on these markers of
                                             // hers not drawn either
    std::vector<SoundReplace::Entry> voice;   // mods: Lara's sounds while this outfit is worn ("voice")
    VoiceEdit::Settings voiceSettings;        // mods: Lara's voice settings while it is worn ("voice_settings")
    std::vector<SoundReplace::Adjustment> foley;   // mods: Lara's foley settings while it is worn ("foley")
    std::vector<SoundReplace::Entry> foleySounds;   // mods: Lara's foley files while it is worn ("foley_sounds")
    std::string foleyFrom;                    // mods: another outfit of the mod whose foley it uses ("foley_from"); its key
    bool gruntsGame = false, foleyGame = false;   // mods: "voice_from" / "foley_from": "<game>": Lara's own with it (not
                                                  // the mod's defaults)
    std::string voiceFrom;                    // mods: another outfit of the mod whose voice (sounds and settings) it
                                              // uses ("voice_from": its id; here its key)
    std::uint16_t baseObject = 0;   // resolved zone IDs (0 = not resolved / unknown)
    std::uint16_t object = 0;
    int profileIndex = -1;          // the object's character profile now (a slot's for a new object; -1: none yet)
    int profileBase = -1;           // the profile it is copied from (its own when the game has one; -1: none: left out)
    bool needsSlot = false;         // a new object: its profile is a slot's (ApplyProfiles, GiveSlot)

    int nameStringId = -1;
    int descriptionStringId = -1;
    bool resolved = false;
    bool reported = false;
    bool pictureReported = false;
    bool profileReported = false;   // (said once: no character profile left for it)
};

struct AppliedOutfit {
    std::size_t config;
    unsigned int listIndex;
    unsigned int baseIndex;
};

// A retail outfit replaced by a mod's outfit ("replaces"): the entry keeps its index, unlock and DLC rules.
struct ReplacedOutfit {
    std::size_t config;
    unsigned int listIndex;
};

std::vector<OutfitConfig> g_configs;

// (v0.9.450) Toggles never shown together: either lists the other in "not_with".
bool NeverShownTogether(const OutfitConfig::Toggle& a, const OutfitConfig::Toggle& b) {
    return std::find(a.notWith.begin(), a.notWith.end(), b.name) != a.notWith.end() ||
           std::find(b.notWith.begin(), b.notWith.end(), a.name) != b.notWith.end();
}
const std::string& ToggleTitle(const OutfitConfig::Toggle& t) { return t.title.empty() ? t.name : t.title; }
// (v0.9.454) The shown toggles that showing toggle `index` of `config` hides (those it is never shown with), by title,
// ", " between ("": none). Under g_toggleMutex once the game runs.
std::string ShownNotWith(const OutfitConfig& config, std::size_t index) {
    std::string titles;
    for (std::size_t k = 0; k < config.toggles.size(); ++k)
        if (k != index && config.toggles[k].on && NeverShownTogether(config.toggles[index], config.toggles[k]))
            titles += (titles.empty() ? "" : ", ") + ToggleTitle(config.toggles[k]);
    return titles;
}
// Mods' game models replaced (mod.json "models"): any object of the game (a weapon, a character, a prop) whose .drm
// the framework's archive overrides with the mod's model built into it (as a retail outfit's replacement is).
struct ModelConfig {
    std::string key;                   // <mod>/<id>
    std::string mod;
    std::string objectName;            // the game's object (its .drm name, lower case)
    fs::path folder;                   // the model's resource files
    std::vector<std::string> exclude;  // files of the folder to leave out (relative, lower case, '/')
    std::vector<fs::path> shared;      // the mod's shared files it uses ("shared"), as if in its folder
};
std::vector<ModelConfig> g_modelConfigs;
std::vector<const char*> g_strings;   // indexed by kStringIdTop - id
int g_stringIdMin = kStringIdTop + 1;
// The campsite Inventory's "Custom parts": two string IDs per part (title, description), from g_partStringTop down.
constexpr int kMaxParts = 64;
int g_partStringTop = -1;

// Tables we gave the game. Old copies are kept: the game (e.g. an open menu) may still point into them.
std::vector<std::unique_ptr<std::uint8_t[]>> g_tableBuffers;
std::uint8_t* g_outfitList = nullptr;
unsigned int g_outfitCount = 0;
unsigned int g_gameOutfitCount = 0;
std::uint8_t* g_profileList = nullptr;
unsigned int g_profileCount = 0;
bool g_profilesApplied = false;   // ApplyProfiles has given every outfit it could a character profile (profileIndex)
// The character profile slots (ApplyProfiles, GiveSlot): profiles past the game's, each given to a new object.
struct ProfileSlot {
    std::string object;        // its object (lower case; empty: free)
    std::uint64_t used = 0;    // when it was last equipped (g_slotClock)
};
std::mutex g_profileMutex;            // the slots, the profile table's entries and count, the configs' profileIndex
std::vector<ProfileSlot> g_slots;     // slot k: profile g_gameProfileCount + k
unsigned int g_gameProfileCount = 0;  // the game's own profiles (90)
std::uint8_t* g_profileUi = nullptr;  // dtp::GlobalUIInfo (its count grows with the slots used)
std::uint64_t g_slotClock = 0;
bool g_slotsLoaded = false;
bool g_slotsOnEquip = false;          // the equip hook is in place: an outfit without a slot gets one when equipped

std::mutex g_appliedMutex;   // g_applied is read by hooks on other threads
std::vector<AppliedOutfit> g_applied;
std::vector<ReplacedOutfit> g_replaced;   // under g_appliedMutex too
std::atomic<int> g_activeCount{0};
std::atomic<int> g_lastWornConfig{-1};
std::string g_defaultGrunts, g_defaultFoley;   // the mods' defaults: the outfits (keys) whose grunts / foley every outfit
                                               // without its own has
fs::path g_statePath;
std::string g_status = "not started";
std::atomic<DWORD> g_gameThreadId{0};

using LocalstrGetFn = const char*(__fastcall*)(int, bool*);
using SetLastOutfitWornFn = void(__fastcall*)(void*, int);
using IsOutfitUnlockedFn = bool(__fastcall*)(void*, unsigned int);
LocalstrGetFn g_origLocalstrGet = nullptr;
SetLastOutfitWornFn g_origSetLastOutfitWorn = nullptr;
IsOutfitUnlockedFn g_origIsOutfitUnlocked = nullptr;

fs::path GameRoot() {
    wchar_t path[MAX_PATH]{};
    const DWORD length = GetModuleFileNameW(nullptr, path, MAX_PATH);
    if (!length || length >= MAX_PATH) return fs::current_path();
    return fs::path(path).parent_path();
}

std::string ReadIni(const fs::path& file, const char* key) {
    char value[1024]{};
    GetPrivateProfileStringA("Outfit", key, "", value, static_cast<DWORD>(sizeof(value)), file.string().c_str());
    std::string text = value;
    while (!text.empty() && (text.back() == ' ' || text.back() == '\t')) text.pop_back();
    return text;
}

// ---- outfit packages: ROTTRFramework\outfits\<Name>.zip -------------------------------------------------------------
// A package is unpacked to outfits\_packages\<Name>\ (again only when the .zip changed) and then used like an
// outfit folder. Its outfit.ini is at the top of the .zip or in one folder (a zipped outfit folder).
constexpr const char* kPackageFormat = "1";

std::string PackageStamp(const fs::path& zip) {
    std::error_code error;
    const auto size = fs::file_size(zip, error);
    const auto time = fs::last_write_time(zip, error).time_since_epoch().count();
    return std::to_string(size) + ":" + std::to_string(static_cast<long long>(time)) + ":" + kPackageFormat;
}

// The manifest (outfit.ini, mod.json) at the top of an unpacked .zip or in its one folder.
fs::path FindManifest(const fs::path& folder, const wchar_t* manifest) {
    std::error_code error;
    if (fs::is_regular_file(folder / manifest, error)) return folder / manifest;
    fs::path found;
    int candidates = 0;
    for (const auto& entry : fs::directory_iterator(folder, error)) {
        if (!entry.is_directory(error)) continue;
        if (fs::is_regular_file(entry.path() / manifest, error)) {
            found = entry.path() / manifest;
            ++candidates;
        }
    }
    return candidates == 1 ? found : fs::path();
}

// The unpacked folder's manifest (outfit.ini / mod.json), or empty (logged) when the package cannot be used.
fs::path PreparePackage(const fs::path& zip, const fs::path& cacheRoot, const wchar_t* manifest = L"outfit.ini") {
    const fs::path destination = cacheRoot / zip.stem();
    const fs::path stampFile = cacheRoot / (zip.stem().wstring() + L".stamp");
    const std::string stamp = PackageStamp(zip);
    std::error_code error;
    {
        std::ifstream in(stampFile);
        std::string previous;
        std::getline(in, previous);
        if (previous == stamp && fs::is_directory(destination, error)) {
            const fs::path ini = FindManifest(destination, manifest);
            if (!ini.empty()) return ini;
        }
    }
    fs::remove(stampFile, error);
    fs::remove_all(destination, error);
    std::string message;
    const DWORD start = GetTickCount();
    if (!ZipReader::ExtractAll(zip, destination, message)) {
        Log::Writef("CustomOutfits: package %s cannot be used: %s", zip.filename().string().c_str(), message.c_str());
        fs::remove_all(destination, error);
        return {};
    }
    const fs::path ini = FindManifest(destination, manifest);
    if (ini.empty()) {
        Log::Writef("CustomOutfits: package %s has no %ls (at the top of the .zip or in one folder)",
                    zip.filename().string().c_str(), manifest);
        fs::remove_all(destination, error);
        return {};
    }
    std::ofstream(stampFile, std::ios::trunc) << stamp << "\n";
    Log::Detailf("CustomOutfits: package %s unpacked (%lu ms)", zip.filename().string().c_str(), GetTickCount() - start);
    return ini;
}

// Unpacked packages whose .zip is gone.
void RemoveStalePackages(const fs::path& outfits, const fs::path& cacheRoot) {
    std::error_code error;
    if (!fs::is_directory(cacheRoot, error)) return;
    std::vector<fs::path> stale;
    for (const auto& entry : fs::directory_iterator(cacheRoot, error)) {
        const bool stampFile = entry.path().extension() == L".stamp";
        const fs::path name = stampFile ? entry.path().stem() : entry.path().filename();
        if (!fs::exists(outfits / (name.wstring() + L".zip"), error)) stale.push_back(entry.path());
    }
    for (const auto& path : stale) fs::remove_all(path, error);
}

bool FolderHasResources(const fs::path& folder) {
    static const char* const kExtensions[] = {".tr10modeldata", ".tr10material", ".tr10dtp", ".dds", ".tr10model",
                                              ".tr10objectref", ".tr10anim", ".tr10animlib", ".tr10cmodel",
                                              ".tr10drivers", ".tr10shaderlib", ".tr10layer", ".tr10contentref"};
    std::error_code error;
    for (auto it = fs::recursive_directory_iterator(folder, error); it != fs::recursive_directory_iterator();
         it.increment(error)) {
        if (error) break;
        if (!it->is_regular_file(error)) continue;
        std::string extension = it->path().extension().string();
        for (char& c : extension) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        for (const char* known : kExtensions)
            if (extension == known) return true;
    }
    return false;
}

// Two outfits must not share a key or build the same new object.
void AddConfig(OutfitConfig config) {
    for (const auto& other : g_configs)
        if (other.key == config.key || (config.buildsModel && other.buildsModel && other.objectName == config.objectName)) {
            Log::Writef("CustomOutfits: %s skipped: %s (remove one of them)", config.key.c_str(),
                        other.key == config.key ? "another outfit has the same name"
                                                : ("its new object " + config.objectName + " is also built by " + other.key).c_str());
            return;
        }
    g_configs.push_back(std::move(config));
}

void LoadConfig(const fs::path& file, const std::string& key, const fs::path& folder, bool package) {
    OutfitConfig config;
    config.key = key;
    config.package = package;
    config.packageFolder = folder;
    config.author = ReadIni(file, "Author");
    config.version = ReadIni(file, "Version");
    config.baseName = ReadIni(file, "Base");
    config.objectName = ReadIni(file, "Object");
    config.name = ReadIni(file, "Name");
    config.description = ReadIni(file, "Description");
    config.model = ReadIni(file, "Model");
    std::string sources = ReadIni(file, "Sources");
    for (std::size_t at = 0; at < sources.size();) {
        std::size_t end = sources.find_first_of(",; ", at);
        if (end == std::string::npos) end = sources.size();
        if (end > at) config.sources.push_back(sources.substr(at, end - at));
        at = end + 1;
    }
    // Archive paths are hashed as written: the game's are lower case.
    auto lower = [](std::string& text) {
        for (char& c : text) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    };
    lower(config.baseName);
    lower(config.objectName);
    lower(config.model);
    for (auto& source : config.sources) lower(source);
    config.folder = folder;
    // Files=: the resource files are somewhere else (absolute, or relative to the .ini's folder).
    const std::string files = ReadIni(file, "Files");
    if (!files.empty()) {
        fs::path filesPath(files);   // ANSI, like GetPrivateProfileStringA
        if (filesPath.is_relative()) filesPath = file.parent_path() / filesPath;
        std::error_code error;
        config.folder = fs::weakly_canonical(filesPath, error);
        if (error) config.folder = filesPath.lexically_normal();
        if (!fs::is_directory(config.folder, error))
            Log::Writef("CustomOutfits: %s: Files=%s is not a folder", key.c_str(), config.folder.string().c_str());
    }
    config.buildsModel = !config.folder.empty() && FolderHasResources(config.folder);
    if (config.baseName.empty() || config.objectName.empty()) {
        Log::Writef("CustomOutfits: %s skipped: Base= and Object= are required", file.string().c_str());
        return;
    }
    AddConfig(std::move(config));
}

// ---- mods: <game>\ROTTRFramework\mods\<Mod>\mod.json (or <Mod>.zip); before 0.9.605 <game>\mods\, still read ---------
// {
//   "name": "...", "author": "...", "version": "...", "description": "...",
//   "outfits": [ { "id": "classic", "object": "laracroft_classic_lito", "base": "laracroft",
//                  "name": "...", "description": "...", "picture": "classic.picture.dds",   (optional)
//                  "model": { "folder": "classic", "from": "laracroft", "sources": [],       (optional)
//                             "exclude": ["textures/1469257.dds"],     (files of the folder to leave out)
//                             "shared": ["shared/1485405.dds"] },      (the mod's files it shares with its other
//                                                                       outfits / models: as if in its folder)
//                  "toggles": [ { "name": "Glasses", "shown": true, "title": "...", "not_with": ["Hat"],
//                                 "parts": [ { "model": 267210, "part": 12, "layout": "9F3A...", "count": 40 } ] } ] } ],
//                  (a toggle's mesh parts: the model's ID (for the Modding Suite), the part record's number, the
//                   model's layout (Features::HiddenModelPart: its copies in memory are found by it) and part count;
//                   v0.9.450 "not_with": the outfit's toggles it is never shown with: v0.9.454 showing it hides those
//                   of them that are shown, and of two shown when the outfit is put on the later one is hidden)
//                  (v0.9.602 a toggle's "holstered": "bow" / "pistol" / "rifle" / "shotgun", v0.9.604 or a list of them:
//                   its parts are drawn only while Lara has a weapon of one of those types holstered (on her holster /
//                   back, not in her hands, not stowed); v0.9.604 "holster_hides": true: hidden while she has instead.
//                   Such a toggle is in no menu (the campsite's Custom Parts and Outfits screen, the framework's): the
//                   holster alone shows or hides it)
//                  "hide_lower_gear": true   (v0.9.455, optional; v0.9.451 "gear": false: Lara's lower gear is not
//                   drawn while the outfit is worn: what she carries on her radio, pistol, axe, glowstick, knife, knife
//                   sheath, rope and belt pouch markers; drawn as usual in her hands. Features::SetPlayerGearHiddenGameThread)
//                  "hide_upper_gear": true   (v0.9.455, optional: the weapons she carries on her back are not drawn:
//                   her bow, rifle and shotgun markers, kUpperGearMarkers)
//                  "hide_markers": [ 294, ... ]   (v0.9.455, optional: whatever she carries on these markers of hers
//                   (laracroft's marker IDs) is not drawn)
//   "overrides": [ { "outfit": "Witch_Base", "name": "...", "description": "...", "unlock": "always" } ],
//   "models": [ { "id": "bow", "object": "wp_bow_broken", "folder": "models/bow", "exclude": [], "shared": [] } ]
// }
// "shared" (the Modding Suite writes it): resource files kept once in the mod (its shared\ folder) for the outfits and
// models that use the same file; each lists the ones it uses, built exactly as if they were in its own folder (a file
// of its folder for the same resource wins).
// "models": game objects whose model (materials, textures) the mod replaces: the object's own .drm is rebuilt with the
// folder's resource files (Blender add-on exports, or only the materials and textures edited).
// An outfit without "model" is a copy of its base under the new object name (or wears "object" when the game has it).
struct RetailOverride {
    std::string mod;
    std::string outfit;          // the outfit's m_debugName, e.g. "Witch_Base"
    std::string name, description;
    bool alwaysUnlocked = false;
    int nameStringId = -1;
    int descriptionStringId = -1;
    bool reported = false;
};
std::vector<RetailOverride> g_overrides;
// Mods' campsite orders (mod.json "order"): retail outfits by m_debugName, the mod's own slots as "slot:<id>" (their
// keys here: <mod>/<id>). Applied in the mods' order, each moving the outfits it names into the places they had.
struct OutfitOrder {
    std::string mod;
    std::vector<std::string> entries;   // m_debugName, or a custom outfit's key
    bool reported = false;
};
std::vector<OutfitOrder> g_orders;
// Mods' outfits removed from the campsite (mod.json "remove": retail outfits by m_debugName): the menu does not list
// them (Crafting::HasOutfit and IsOutfitCraftable say no); their entries stay in the list (indexes unchanged).
struct RemovedOutfit {
    std::string mod;
    std::string name;
    bool reported = false;
};
std::vector<RemovedOutfit> g_removed;
std::vector<unsigned int> g_removedIndexes;   // their list indexes (g_appliedMutex)
std::vector<unsigned int> g_forcedUnlocked;   // outfit-list indexes of overridden retail outfits ("unlock": "always")

fs::path PathFromUtf8(const std::string& text) { return fs::path(std::u8string(text.begin(), text.end())); }

std::string JsonString(const nlohmann::json& object, const char* key) {
    auto it = object.find(key);
    return it != object.end() && it->is_string() ? it->get<std::string>() : std::string();
}

std::string LowerAscii(std::string text) {
    for (char& c : text) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return text;
}

// mod.json sound lists ("waves", an outfit's "voice"): [ { "wave": 1234, "files": ["sounds/a.wav", ...], "label": "..." } ];
// an outfit's voice can also list streams: { "stream": "6DA8F25D" (the game file's name hash), "files": [".mul", ...] }
std::vector<SoundReplace::Entry> ParseWaves(const nlohmann::json& list, const fs::path& folder, const std::string& key) {
    std::vector<SoundReplace::Entry> entries;
    if (!list.is_array()) return entries;
    for (const auto& e : list) {
        if (!e.is_object()) continue;
        SoundReplace::Entry entry;
        if (auto wave = e.find("wave"); wave != e.end() && (wave->is_number_unsigned() || wave->is_number_integer()))
            entry.wave = static_cast<std::uint32_t>(wave->get<std::int64_t>());
        if (const std::string stream = JsonString(e, "stream"); !stream.empty()) {
            entry.stream = true;
            entry.wave = static_cast<std::uint32_t>(std::strtoul(stream.c_str(), nullptr, 16));
        }
        entry.label = JsonString(e, "label");
        if (auto language = e.find("language"); language != e.end()) {
            entry.language = language->is_string() ? SoundReplace::LanguageFromName(language->get<std::string>())
                             : language->is_number_integer() ? int(language->get<std::int64_t>()) : -1;
            if (entry.language < 0) {
                Log::Writef("Sounds: %s: wave %u: the language %s is not one of the game's", key.c_str(), entry.wave, language->dump().c_str());
                continue;
            }
        }
        if (auto files = e.find("files"); files != e.end() && files->is_array())
            for (const auto& f : *files)
                if (f.is_string()) {
                    std::error_code ec;
                    const fs::path path = folder / PathFromUtf8(f.get<std::string>());
                    if (fs::is_regular_file(path, ec)) entry.files.push_back(path);
                    else Log::Writef("Sounds: %s: %s is missing", key.c_str(), f.get<std::string>().c_str());
                }
        // (a language's entry without files: that language plays the game's own, not the entry for every language)
        if (entry.wave && (!entry.files.empty() || entry.language >= 0)) entries.push_back(std::move(entry));
    }
    return entries;
}

// Mods' sound streams (mod.json "streams": .mul files the Modding Suite built, by the game file's name hash), put into
// the framework's archive.
std::vector<OutfitArchive::RawFile> g_streams;

// An outfit's voice streams: each file goes into the framework's archive as a stream of our own
// (audio\streams\rmsvoice\v<n>\<game hash>_<file>), the name the sound hook gives the game instead.
// mod.json "voice_settings": { "values": [ { "dtp", "offset", "type", "value", "expect": [...], "size" } ],
// "chances": [ { "vocals", "bark", "chance" } ] } (the Modding Suite's voice editor)
VoiceEdit::Settings ParseVoiceSettings(const nlohmann::json& settings) {
    VoiceEdit::Settings out;
    auto number = [](const nlohmann::json& o, const char* key) -> std::uint32_t {
        const auto it = o.find(key);
        return it != o.end() && it->is_number_unsigned() ? it->get<std::uint32_t>() : 0u;
    };
    auto text = [](const nlohmann::json& o, const char* key) -> std::string {
        const auto it = o.find(key);
        return it != o.end() && it->is_string() ? it->get<std::string>() : std::string();
    };
    if (auto values = settings.find("values"); values != settings.end() && values->is_array())
        for (const auto& v : *values) {
            if (!v.is_object()) continue;
            VoiceEdit::Value x;
            x.dtp = number(v, "dtp");
            x.offset = number(v, "offset");
            x.type = text(v, "type");
            x.value = text(v, "value");
            x.size = number(v, "size");
            if (auto choice = v.find("choice"); choice != v.end() && choice->is_boolean()) x.choice = choice->get<bool>();
            if (auto expect = v.find("expect"); expect != v.end() && expect->is_array())
                for (const auto& e : *expect)
                    if (e.is_string()) x.expect.push_back(e.get<std::string>());
            if (x.dtp && !x.type.empty() && !x.value.empty()) out.values.push_back(std::move(x));
        }
    if (auto chances = settings.find("chances"); chances != settings.end() && chances->is_array())
        for (const auto& c : *chances) {
            if (!c.is_object()) continue;
            VoiceEdit::Chance x;
            x.vocals = number(c, "vocals");
            x.bark = number(c, "bark");
            if (auto chance = c.find("chance"); chance != c.end() && chance->is_number()) x.chance = chance->get<float>();
            if (x.vocals) out.chances.push_back(x);
        }
    return out;
}

void AddVoiceStreams(std::vector<SoundReplace::Entry>& voice, const std::string& key) {
    static int outfit = 0;
    bool any = false;
    std::map<std::string, std::string> byFile;   // (a file several streams play: one stream in the archive)
    for (auto& entry : voice) {
        if (!entry.stream) continue;
        any = true;
        for (std::size_t i = 0; i < entry.files.size(); ++i) {
            const std::string path = entry.files[i].lexically_normal().string();
            if (auto it = byFile.find(path); it != byFile.end()) {
                entry.names.push_back(it->second);
                continue;
            }
            char name[64];
            if (entry.language >= 0) std::snprintf(name, sizeof(name), "rmsvoice\\v%d\\%08x_l%d_%zu", outfit, entry.wave, entry.language, i);
            else std::snprintf(name, sizeof(name), "rmsvoice\\v%d\\%08x_%zu", outfit, entry.wave, i);
            OutfitArchive::RawFile file;
            file.hash = SoundReplace::StreamHash(name);
            file.source = entry.files[i];
            file.label = key + "/voice " + (entry.label.empty() ? std::string(name) : entry.label);
            g_streams.push_back(std::move(file));
            entry.names.push_back(name);
            byFile.emplace(path, name);
        }
    }
    if (any) ++outfit;
}

// "shared": the mod's files (paths in the mod) an outfit or model uses as if they were in its folder.
std::vector<fs::path> ParseShared(const nlohmann::json& entry, const fs::path& folder, const std::string& key) {
    std::vector<fs::path> out;
    auto list = entry.find("shared");
    if (list == entry.end() || !list->is_array()) return out;
    std::size_t missing = 0;
    for (const auto& file : *list) {
        if (!file.is_string()) continue;
        std::error_code ec;
        const fs::path path = folder / PathFromUtf8(file.get<std::string>());
        if (fs::is_regular_file(path, ec)) out.push_back(path);
        else ++missing;
    }
    if (missing) Log::Writef("CustomOutfits: %s: %zu of its shared files are missing", key.c_str(), missing);
    return out;
}

void LoadMod(const fs::path& file, const std::string& key, const fs::path& folder, bool package) {
    std::ifstream in(file, std::ios::binary);
    const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    const nlohmann::json mod = nlohmann::json::parse(text, nullptr, false, true);
    if (mod.is_discarded() || !mod.is_object()) {
        Log::Writef("CustomOutfits: mod %s skipped: its mod.json is not valid JSON", key.c_str());
        return;
    }
    int outfits = 0, overrides = 0, models = 0;
    try {
        if (auto list = mod.find("models"); list != mod.end() && list->is_array())
            for (const auto& entry : *list) {
                if (!entry.is_object()) continue;
                ModelConfig config;
                config.mod = key;
                config.objectName = LowerAscii(JsonString(entry, "object"));
                const std::string id = JsonString(entry, "id");
                config.key = key + "/" + (id.empty() ? config.objectName : id);
                const std::string sub = JsonString(entry, "folder");
                if (!sub.empty()) config.folder = folder / PathFromUtf8(sub);
                if (auto exclude = entry.find("exclude"); exclude != entry.end() && exclude->is_array())
                    for (const auto& file : *exclude)
                        if (file.is_string()) {
                            std::string name = LowerAscii(file.get<std::string>());
                            std::replace(name.begin(), name.end(), '\\', '/');
                            config.exclude.push_back(name);
                        }
                config.shared = ParseShared(entry, folder, config.key);
                if (config.objectName.empty() || config.folder.empty() || (!FolderHasResources(config.folder) && config.shared.empty())) {
                    Log::Writef("CustomOutfits: model %s skipped: it needs \"object\" and a \"folder\" with resource files",
                                config.key.c_str());
                    continue;
                }
                const auto taken = std::find_if(g_modelConfigs.begin(), g_modelConfigs.end(),
                                                [&](const ModelConfig& other) { return other.objectName == config.objectName; });
                if (taken != g_modelConfigs.end()) {
                    Log::Writef("CustomOutfits: model %s skipped: %s is already replaced by %s", config.key.c_str(),
                                config.objectName.c_str(), taken->key.c_str());
                    continue;
                }
                g_modelConfigs.push_back(std::move(config));
                ++models;
            }
        if (auto list = mod.find("outfits"); list != mod.end() && list->is_array())
            for (const auto& entry : *list) {
                if (!entry.is_object()) continue;
                OutfitConfig config;
                config.mod = key;
                config.objectName = LowerAscii(JsonString(entry, "object"));
                config.replaces = JsonString(entry, "replaces");
                const std::string id = JsonString(entry, "id");
                config.key = key + "/" + (id.empty() ? config.objectName : id);
                config.baseName = LowerAscii(JsonString(entry, "base"));
                config.name = JsonString(entry, "name");
                config.description = JsonString(entry, "description");
                config.author = JsonString(mod, "author");
                config.version = JsonString(mod, "version");
                config.package = package;
                config.packageFolder = folder;
                if (auto model = entry.find("model"); model != entry.end() && model->is_object()) {
                    config.folder = folder / PathFromUtf8(JsonString(*model, "folder"));
                    config.model = LowerAscii(JsonString(*model, "from"));
                    if (auto sources = model->find("sources"); sources != model->end() && sources->is_array())
                        for (const auto& source : *sources)
                            if (source.is_string()) config.sources.push_back(LowerAscii(source.get<std::string>()));
                    if (auto exclude = model->find("exclude"); exclude != model->end() && exclude->is_array())
                        for (const auto& file : *exclude)
                            if (file.is_string()) {
                                std::string name = LowerAscii(file.get<std::string>());
                                std::replace(name.begin(), name.end(), '\\', '/');
                                config.exclude.push_back(name);
                            }
                    config.shared = ParseShared(*model, folder, config.key);
                    config.buildsModel = FolderHasResources(config.folder) || !config.shared.empty();
                    if (!config.buildsModel)
                        Log::Writef("CustomOutfits: %s: its model folder has no resource files (a copy of %s is used)",
                                    config.key.c_str(), config.baseName.c_str());
                }
                if (const std::string picture = JsonString(entry, "picture"); !picture.empty()) {
                    std::error_code ec;
                    const fs::path path = folder / PathFromUtf8(picture);
                    if (fs::is_regular_file(path, ec)) config.picture = path;
                    else Log::Writef("CustomOutfits: %s: its picture %s is missing", config.key.c_str(), picture.c_str());
                }
                // A replacement without a model wears the retail outfit's own object (only its picture, toggles
                // or text change): nothing is built for it. With another "object": that one.
                // ("object" another name: a copy of its base under that name, or the game's object of that name, worn)
                if (!config.replaces.empty() && !config.buildsModel && config.objectName.empty()) config.objectName = config.baseName;
                if (!config.buildsModel) {
                    config.buildsModel = true;
                    config.cloneOnly = true;
                    config.folder.clear();
                }
                if (auto voice = entry.find("voice"); voice != entry.end()) {
                    config.voice = ParseWaves(*voice, folder, config.key);
                    AddVoiceStreams(config.voice, config.key);
                }
                if (auto settings = entry.find("voice_settings"); settings != entry.end() && settings->is_object())
                    config.voiceSettings = ParseVoiceSettings(*settings);
                // "foley": [ { "waves": [1234, ...], "volume": 0.5, "pitch": -2, "label": "..." } ] (the Modding Suite's
                // Foley: footsteps, clothing... of the outfit's object, by group; volume 0: not heard)
                if (auto foley = entry.find("foley"); foley != entry.end() && foley->is_array())
                    for (const auto& f : *foley) {
                        if (!f.is_object()) continue;
                        SoundReplace::Adjustment a;
                        if (auto v = f.find("volume"); v != f.end() && v->is_number()) a.volume = v->get<float>();
                        if (auto p = f.find("pitch"); p != f.end() && p->is_number()) a.pitch = p->get<float>();
                        if (auto waves = f.find("waves"); waves != f.end() && waves->is_array())
                            for (const auto& w : *waves)
                                if (w.is_number_unsigned() || w.is_number_integer()) {
                                    a.wave = static_cast<std::uint32_t>(w.get<std::int64_t>());
                                    config.foley.push_back(a);
                                }
                    }
                if (const std::string from = JsonString(entry, "voice_from"); from == "<game>") config.gruntsGame = true;
                else if (!from.empty()) config.voiceFrom = key + "/" + from;
                if (const std::string from = JsonString(entry, "foley_from"); from == "<game>") config.foleyGame = true;
                else if (!from.empty()) config.foleyFrom = key + "/" + from;
                if (auto sounds = entry.find("foley_sounds"); sounds != entry.end()) {
                    config.foleySounds = ParseWaves(*sounds, folder, config.key);
                    AddVoiceStreams(config.foleySounds, config.key);
                }
                if (auto gear = entry.find("gear"); gear != entry.end() && gear->is_boolean()) config.hideLowerGear = !gear->get<bool>();
                if (auto hide = entry.find("hide_lower_gear"); hide != entry.end() && hide->is_boolean())
                    config.hideLowerGear = hide->get<bool>();
                if (auto hide = entry.find("hide_upper_gear"); hide != entry.end() && hide->is_boolean())
                    config.hideUpperGear = hide->get<bool>();
                if (auto hide = entry.find("hide_markers"); hide != entry.end() && hide->is_array())
                    for (const auto& id : *hide)
                        if (id.is_number_integer() && id.get<std::int64_t>() > 0 && id.get<std::int64_t>() < 65536)
                            config.hideMarkers.push_back(static_cast<std::int32_t>(id.get<std::int64_t>()));
                if (auto toggles = entry.find("toggles"); toggles != entry.end() && toggles->is_array())
                    for (const auto& t : *toggles) {
                        if (!t.is_object()) continue;
                        OutfitConfig::Toggle toggle;
                        toggle.name = JsonString(t, "name");
                        if (auto parts = t.find("parts"); parts != t.end() && parts->is_array())
                            for (const auto& part : *parts) {
                                if (!part.is_object()) continue;
                                Features::HiddenModelPart p;
                                p.layout = std::strtoull(JsonString(part, "layout").c_str(), nullptr, 16);
                                if (auto n = part.find("part"); n != part.end() && n->is_number_unsigned()) p.part = n->get<std::uint32_t>();
                                else continue;
                                if (auto n = part.find("count"); n != part.end() && n->is_number_unsigned()) p.parts = n->get<std::uint32_t>();
                                if (p.layout && p.parts && p.part < p.parts) toggle.parts.push_back(p);
                            }
                        if (auto shown = t.find("shown"); shown != t.end() && shown->is_boolean()) toggle.on = shown->get<bool>();
                        toggle.title = JsonString(t, "title");
                        if (auto list = t.find("not_with"); list != t.end() && list->is_array())
                            for (const auto& n : *list)
                                if (n.is_string() && !n.get<std::string>().empty() && n.get<std::string>() != toggle.name)
                                    toggle.notWith.push_back(n.get<std::string>());
                        {   // (v0.9.602: one weapon type; v0.9.604: a list too, and "holster_hides")
                            static const char* const kHolstered[] = {"bow", "pistol", "rifle", "shotgun"};   // (WeaponClass order)
                            std::vector<std::string> named;
                            if (auto h = t.find("holstered"); h != t.end()) {
                                if (h->is_string()) named.push_back(h->get<std::string>());
                                else if (h->is_array())
                                    for (const auto& n : *h)
                                        if (n.is_string()) named.push_back(n.get<std::string>());
                            }
                            for (const auto& n : named) {
                                const std::string weapon = LowerAscii(n);
                                bool known = false;
                                for (int w = 0; w < 4; ++w)
                                    if (weapon == kHolstered[w]) toggle.holstered |= 1u << w, known = true;
                                if (!known && !weapon.empty())
                                    Log::Writef("CustomOutfits: %s: toggle %s: \"holstered\": \"%s\" is not bow, pistol, rifle or "
                                                "shotgun (left out)", config.key.c_str(), toggle.name.c_str(), weapon.c_str());
                            }
                            if (auto hides = t.find("holster_hides"); hides != t.end() && hides->is_boolean())
                                toggle.holsterHides = hides->get<bool>();
                            if (toggle.holstered) toggle.on = true, toggle.notWith.clear();   // (no menu: always on)
                        }
                        if (!toggle.name.empty() && !toggle.parts.empty()) config.toggles.push_back(std::move(toggle));
                    }
                // (v0.9.604) the toggles following a holster: never shown together with none (no menu shows them)
                for (auto& toggle : config.toggles)
                    toggle.notWith.erase(std::remove_if(toggle.notWith.begin(), toggle.notWith.end(), [&](const std::string& name) {
                                             return std::any_of(config.toggles.begin(), config.toggles.end(), [&](const auto& o) {
                                                 return o.holstered && o.name == name;
                                             });
                                         }),
                                         toggle.notWith.end());
                for (std::size_t k = 0; k < config.toggles.size(); ++k)
                    if (!config.toggles[k].holstered) config.menuToggles.push_back(k);
                if (config.baseName.empty() || config.objectName.empty()) {
                    Log::Writef("CustomOutfits: %s skipped: \"base\" and \"object\" are required", config.key.c_str());
                    continue;
                }
                if (!config.replaces.empty() && std::any_of(g_configs.begin(), g_configs.end(), [&](const OutfitConfig& other) {
                        return _stricmp(other.replaces.c_str(), config.replaces.c_str()) == 0;
                    })) {
                    Log::Writef("CustomOutfits: %s skipped: outfit %s is already replaced by %s", config.key.c_str(),
                                config.replaces.c_str(),
                                std::find_if(g_configs.begin(), g_configs.end(), [&](const OutfitConfig& other) {
                                    return _stricmp(other.replaces.c_str(), config.replaces.c_str()) == 0;
                                })->key.c_str());
                    continue;
                }
                AddConfig(std::move(config));
                ++outfits;
            }
        // grunts and foley of other outfits ("voice_from", "foley_from"): their files (the same, loaded once) and settings
        auto source = [&](const OutfitConfig& config, std::string OutfitConfig::*link, const char* what) -> const OutfitConfig* {
            const OutfitConfig* from = &config;
            for (int hops = 0; from && !(from->*link).empty() && hops < 8; ++hops) {
                const std::string next = from->*link;
                from = nullptr;
                for (const auto& other : g_configs)
                    if (other.key == next) from = &other;
            }
            if (!from || from == &config || !(from->*link).empty()) {
                Log::Writef("CustomOutfits: %s: the outfit whose %s it uses (%s) is not in the mod", config.key.c_str(), what,
                            (config.*link).c_str());
                return nullptr;
            }
            return from;
        };
        for (auto& config : g_configs) {
            if (config.mod != key) continue;
            if (!config.voiceFrom.empty())
                if (const OutfitConfig* from = source(config, &OutfitConfig::voiceFrom, "grunts")) {
                    config.voice = from->voice;
                    config.voiceSettings = from->voiceSettings;
                    Log::Detailf("CustomOutfits: %s uses the grunts of %s (%zu sound(s), %zu setting(s))", config.key.c_str(),
                                from->key.c_str(), config.voice.size(),
                                config.voiceSettings.values.size() + config.voiceSettings.chances.size());
                }
            if (!config.foleyFrom.empty())
                if (const OutfitConfig* from = source(config, &OutfitConfig::foleyFrom, "foley")) {
                    config.foley = from->foley;
                    config.foleySounds = from->foleySounds;
                    Log::Detailf("CustomOutfits: %s uses the foley of %s (%zu sound(s), %zu setting(s))", config.key.c_str(),
                                from->key.c_str(), config.foleySounds.size(), config.foley.size());
                }
        }
        // the mod's defaults: the outfit whose grunts / foley every outfit without its own has ("default_grunts",
        // "default_foley": its id), the game's own outfits too
        for (const auto& [field, target] : {std::pair<const char*, std::string*>{"default_grunts", &g_defaultGrunts},
                                            std::pair<const char*, std::string*>{"default_foley", &g_defaultFoley}}) {
            const std::string id = JsonString(mod, field);
            if (id.empty()) continue;
            const std::string wanted = key + "/" + id;
            if (std::none_of(g_configs.begin(), g_configs.end(), [&](const OutfitConfig& c) { return c.key == wanted; })) {
                Log::Writef("CustomOutfits: mod %s: %s: no outfit %s in the mod", key.c_str(), field, id.c_str());
                continue;
            }
            if (!target->empty()) {
                Log::Detailf("CustomOutfits: mod %s: %s: another mod's (%s) is used", key.c_str(), field, target->c_str());
                continue;
            }
            *target = wanted;
            Log::Detailf("CustomOutfits: every outfit without its own uses the %s of %s", field + 8, wanted.c_str());
        }
        if (auto list = mod.find("waves"); list != mod.end()) SoundReplace::AddGlobal(ParseWaves(*list, folder, key), key);
        // "subtitles": [ { "cutscene", "stream", "language", "frame", "original", "text" } ] (the Modding Suite's
        // subtitle editor: a cutscene's block in a text language, by the text the game has there)
        if (auto list = mod.find("subtitles"); list != mod.end() && list->is_array()) {
            std::vector<SubtitleEdit::Edit> edits;
            for (const auto& entry : *list) {
                if (!entry.is_object()) continue;
                SubtitleEdit::Edit e;
                e.language = SoundReplace::LanguageFromName(JsonString(entry, "language"));
                e.original = JsonString(entry, "original");
                e.text = JsonString(entry, "text");
                e.cinematic = JsonString(entry, "stream");
                e.mod = key;
                if (e.language < 0 || e.original.empty()) {
                    Log::Writef("Subtitles: %s: a subtitle skipped (no language or no original text)", key.c_str());
                    continue;
                }
                edits.push_back(std::move(e));
            }
            SubtitleEdit::Add(edits);
        }
        if (auto list = mod.find("streams"); list != mod.end() && list->is_array())
            for (const auto& entry : *list) {
                if (!entry.is_object()) continue;
                OutfitArchive::RawFile stream;
                const std::string hash = JsonString(entry, "hash");
                stream.hash = static_cast<std::uint32_t>(std::strtoul(hash.c_str(), nullptr, 16));
                stream.source = folder / PathFromUtf8(JsonString(entry, "file"));
                stream.label = key + "/" + (JsonString(entry, "label").empty() ? hash : JsonString(entry, "label"));
                std::error_code ec;
                if (!stream.hash || !fs::is_regular_file(stream.source, ec)) {
                    Log::Writef("Sounds: %s: stream %s skipped (no file)", key.c_str(), hash.c_str());
                    continue;
                }
                g_streams.push_back(std::move(stream));
            }
        if (auto list = mod.find("overrides"); list != mod.end() && list->is_array())
            for (const auto& entry : *list) {
                if (!entry.is_object()) continue;
                RetailOverride o;
                o.mod = key;
                o.outfit = JsonString(entry, "outfit");
                o.name = JsonString(entry, "name");
                o.description = JsonString(entry, "description");
                o.alwaysUnlocked = LowerAscii(JsonString(entry, "unlock")) == "always";
                if (o.outfit.empty()) continue;
                g_overrides.push_back(std::move(o));
                ++overrides;
            }
        if (auto list = mod.find("remove"); list != mod.end() && list->is_array())
            for (const auto& entry : *list)
                if (entry.is_string() && !entry.get<std::string>().empty()) g_removed.push_back({key, entry.get<std::string>()});
        if (auto list = mod.find("order"); list != mod.end() && list->is_array()) {
            OutfitOrder order;
            order.mod = key;
            for (const auto& entry : *list) {
                if (!entry.is_string()) continue;
                std::string name = entry.get<std::string>();
                if (name.rfind("slot:", 0) == 0) name = key + "/" + name.substr(5);
                if (!name.empty()) order.entries.push_back(std::move(name));
            }
            if (!order.entries.empty()) g_orders.push_back(std::move(order));
        }
        // (v0.9.565) "weapons", "upgrades", "values": the Modding Suite's Weapons tab
        WeaponMods::ParseMod(mod, key, folder);
    } catch (const std::exception& e) {
        Log::Writef("CustomOutfits: mod %s: %s", key.c_str(), e.what());
    }
    Log::Writef("CustomOutfits: mod %s%s: %d outfit(s), %d override(s), %d model(s)%s%s", key.c_str(),
                package ? " (.zip)" : "", outfits, overrides, models, JsonString(mod, "version").empty() ? "" : ", version ",
                JsonString(mod, "version").c_str());
}

// (0.9.605) The mods are in <game>\ROTTRFramework\mods (the Modding Suite's since 2.0.139, which moves the old ones
// there); <game>\mods, where they were, is read after it: a mod of a name ROTTRFramework\mods has already is left out.
void LoadMods() {
    const fs::path roots[2] = {GameRoot() / L"ROTTRFramework" / L"mods", GameRoot() / L"mods"};
    const char* const labels[2] = {"ROTTRFramework\\mods\\", "mods\\"};
    Log::Write("CustomOutfits: mods read from ROTTRFramework\\mods\\ (then from the game folder's mods\\, where they were before)");
    std::vector<std::string> loaded;   // (the mods' names: one of each)
    auto taken = [&](const std::string& name) {
        return std::any_of(loaded.begin(), loaded.end(), [&](const std::string& n) { return _stricmp(n.c_str(), name.c_str()) == 0; });
    };
    for (int r = 0; r < 2; ++r) {
        const fs::path& root = roots[r];
        const char* label = labels[r];
        std::error_code error;
        if (!fs::is_directory(root, error)) continue;
        std::vector<std::pair<std::string, fs::path>> folders;
        std::vector<fs::path> zips;
        for (const auto& entry : fs::directory_iterator(root, error)) try {
            const std::string name = entry.path().filename().string();   // throws for names the ANSI code page lacks
            if (name.empty() || name[0] == '_') continue;                 // "_Name": disabled
            if (entry.is_directory(error) && fs::is_regular_file(entry.path() / L"mod.json", error))
                folders.push_back({name, entry.path()});
            else if (entry.is_regular_file(error) && _wcsicmp(entry.path().extension().c_str(), L".zip") == 0)
                zips.push_back(entry.path());
        } catch (const std::exception&) {
            Log::Write("CustomOutfits: a mod was skipped: its name has characters outside the system's code page (rename it)");
        }
        std::sort(folders.begin(), folders.end());
        std::sort(zips.begin(), zips.end());
        if (r == 1 && (!folders.empty() || !zips.empty()))
            Log::Writef("CustomOutfits: %zu mod(s) still in the game folder's mods\\ (the Modding Suite moves them into "
                        "ROTTRFramework\\mods\\)", folders.size() + zips.size());
        for (const auto& [name, folder] : folders) try {
            if (taken(name)) {
                Log::Writef("CustomOutfits: %s%s skipped: ROTTRFramework\\mods\\ has a mod of that name", label, name.c_str());
                continue;
            }
            loaded.push_back(name);
            LoadMod(folder / L"mod.json", name, folder, false);
        } catch (const std::exception& e) {
            Log::Writef("CustomOutfits: mod %s skipped: %s", name.c_str(), e.what());
        }
        const fs::path cacheRoot = root / L"_cache";
        RemoveStalePackages(root, cacheRoot);
        for (const auto& zip : zips) try {
            const std::string name = zip.stem().string();
            if (std::any_of(folders.begin(), folders.end(), [&](const auto& f) { return _stricmp(f.first.c_str(), name.c_str()) == 0; })) {
                Log::Writef("CustomOutfits: %s%s.zip skipped: the folder %s%s has the same name", label, name.c_str(), label, name.c_str());
                continue;
            }
            if (taken(name)) {
                Log::Writef("CustomOutfits: %s%s.zip skipped: ROTTRFramework\\mods\\ has a mod of that name", label, name.c_str());
                continue;
            }
            loaded.push_back(name);
            const fs::path manifest = PreparePackage(zip, cacheRoot, L"mod.json");
            if (!manifest.empty()) LoadMod(manifest, name, manifest.parent_path(), true);
        } catch (const std::exception& e) {
            Log::Writef("CustomOutfits: a mod .zip was skipped: %s", e.what());
        }
    }
}

void LoadLegacyConfigs(const fs::path& folder);
void AssignStrings();

void LoadConfigs() {
    const fs::path folder = GameRoot() / L"ROTTRFramework" / L"outfits";
    g_statePath = folder / L"_state.ini";
    LoadLegacyConfigs(folder);   // ROTTRFramework\outfits\*.ini, folders, .zip (the earlier format)
    LoadMods();
    AssignStrings();
}

void LoadLegacyConfigs(const fs::path& folder) {
    std::error_code error;
    if (!fs::is_directory(folder, error)) return;
    struct Found {
        std::string key;
        fs::path ini;
        fs::path folder;
        bool package = false;
    };
    std::vector<Found> found;
    std::vector<fs::path> packages;
    for (const auto& entry : fs::directory_iterator(folder, error)) try {
        const std::string stem = entry.path().stem().string();   // throws for names the ANSI code page lacks
        if (stem.empty() || stem[0] == '_') continue;
        if (entry.is_directory(error)) {
            const fs::path ini = entry.path() / L"outfit.ini";
            if (fs::is_regular_file(ini, error)) found.push_back({entry.path().filename().string(), ini, entry.path()});
        } else if (entry.is_regular_file(error) && entry.path().extension() == L".ini") {
            found.push_back({stem, entry.path(), fs::path()});
        } else if (entry.is_regular_file(error) && _wcsicmp(entry.path().extension().c_str(), L".zip") == 0) {
            packages.push_back(entry.path());
        }
    } catch (const std::exception&) {
        Log::Write("CustomOutfits: an entry in the outfits folder was skipped: its name has characters outside the "
                   "system's code page (rename it)");
    }
    // Loose folders and .ini files first: a working copy wins over a package of the same name.
    std::sort(found.begin(), found.end(), [](const auto& a, const auto& b) { return a.key < b.key; });
    std::sort(packages.begin(), packages.end());
    const fs::path cacheRoot = folder / L"_packages";
    RemoveStalePackages(folder, cacheRoot);
    for (const auto& zip : packages) try {
        const fs::path ini = PreparePackage(zip, cacheRoot);
        if (!ini.empty()) found.push_back({zip.stem().string(), ini, ini.parent_path(), true});
    } catch (const std::exception& e) {
        Log::Writef("CustomOutfits: a package was skipped: %s", e.what());
    }
    for (const auto& item : found) try {
        LoadConfig(item.ini, item.key, item.folder, item.package);
    } catch (const std::exception& e) {
        Log::Writef("CustomOutfits: %s skipped: %s", item.key.c_str(), e.what());
    }
}

void AssignStrings() {
    // String IDs for names and descriptions (custom outfits and overridden retail ones).
    int next = kStringIdTop;
    for (auto& config : g_configs) {
        if (!config.name.empty()) config.nameStringId = next--;
        if (!config.description.empty()) config.descriptionStringId = next--;
    }
    for (auto& o : g_overrides) {
        if (!o.name.empty()) o.nameStringId = next--;
        if (!o.description.empty()) o.descriptionStringId = next--;
    }
    const std::vector<WeaponMods::Text*> weaponTexts = WeaponMods::Texts();   // (v0.9.565) mods' weapons and upgrades
    for (auto* t : weaponTexts) t->id = next--;
    g_partStringTop = next;
    next -= kMaxParts * 2;
    g_stringIdMin = next + 1;
    g_strings.assign(static_cast<std::size_t>(kStringIdTop - next), nullptr);
    for (auto& config : g_configs) {
        if (config.nameStringId >= 0) g_strings[kStringIdTop - config.nameStringId] = config.name.c_str();
        if (config.descriptionStringId >= 0)
            g_strings[kStringIdTop - config.descriptionStringId] = config.description.c_str();
    }
    for (auto& o : g_overrides) {
        if (o.nameStringId >= 0) g_strings[kStringIdTop - o.nameStringId] = o.name.c_str();
        if (o.descriptionStringId >= 0) g_strings[kStringIdTop - o.descriptionStringId] = o.description.c_str();
    }
    for (auto* t : weaponTexts) g_strings[static_cast<std::size_t>(kStringIdTop - t->id)] = t->text.c_str();

    char last[256]{};
    GetPrivateProfileStringA("State", "LastWorn", "", last, sizeof(last), g_statePath.string().c_str());
    for (std::size_t i = 0; i < g_configs.size(); ++i)
        if (g_configs[i].key == last) g_lastWornConfig.store(static_cast<int>(i));
    Log::Writef("CustomOutfits: %zu custom outfit(s) and %zu retail override(s) configured (ROTTRFramework\\mods\\ and "
                "ROTTRFramework\\outfits\\)", g_configs.size(), g_overrides.size());
    for (std::size_t i = 0; i < g_configs.size(); ++i) {
        auto& config = g_configs[i];
        for (auto& toggle : config.toggles) {
            const std::string key = config.key + "/" + toggle.name;
            const int saved = GetPrivateProfileIntA("Toggles", key.c_str(), -1, g_statePath.string().c_str());
            if (saved >= 0 && !toggle.holstered) toggle.on = saved != 0;   // (v0.9.604: one following a holster: always on)
        }
        // (v0.9.450) of two shown toggles never shown together, the later one starts hidden
        for (std::size_t t = 0; t < config.toggles.size(); ++t)
            for (std::size_t k = 0; k < t && config.toggles[t].on; ++k)
                if (config.toggles[k].on && NeverShownTogether(config.toggles[t], config.toggles[k])) {
                    config.toggles[t].on = false;
                    Log::Writef("CustomOutfits: %s: %s is never shown with %s: it starts hidden", config.key.c_str(),
                                config.toggles[t].name.c_str(), config.toggles[k].name.c_str());
                }
        for (const auto& toggle : config.toggles)   // (v0.9.602; the Modding Suite looks for these texts in the .asi)
            if (toggle.holstered) {
                std::string weapons;
                static const char* const kNames[] = {"bow", "pistol", "rifle", "shotgun"};
                for (int w = 0; w < 4; ++w)
                    if (toggle.holstered & (1u << w)) weapons += std::string(weapons.empty() ? "" : " or ") + kNames[w];
                if (toggle.holsterHides)
                    Log::Writef("CustomOutfits: %s: %s hidden while the %s is holstered (not in the menus)", config.key.c_str(),
                                toggle.name.c_str(), weapons.c_str());
                else
                    Log::Writef("CustomOutfits: %s: %s shown only while the %s is holstered (not in the menus)", config.key.c_str(),
                                toggle.name.c_str(), weapons.c_str());
            }
        if (config.package || !config.toggles.empty())
            Log::Detailf("CustomOutfits:   %s%s%s%s%s, %zu toggle(s)", config.key.c_str(), config.package ? " (package" : "",
                        config.package && !config.version.empty() ? " v" : "", config.package ? config.version.c_str() : "",
                        config.package ? ")" : "", config.toggles.size());
    }
}

void SaveLastWorn(int config) {
    if (g_lastWornConfig.exchange(config) == config) return;
    const std::string value = config >= 0 ? g_configs[static_cast<std::size_t>(config)].key : std::string();
    std::error_code error;
    fs::create_directories(g_statePath.parent_path(), error);
    WritePrivateProfileStringA("State", "LastWorn", value.c_str(), g_statePath.string().c_str());
}

// The length of the game's own object (zone) table, as seen at runtime: the archive builder starts the new objects'
// zone IDs above it (zonelist.ids alone can be shorter than what the game ends up with).
std::uint32_t KnownZoneCount() {
    return GetPrivateProfileIntA("Zones", "GameCount", 0, g_statePath.string().c_str());
}
void SaveZoneCount(std::uint32_t count) {
    if (!count || KnownZoneCount() == count) return;
    std::error_code error;
    fs::create_directories(g_statePath.parent_path(), error);
    WritePrivateProfileStringA("Zones", "GameCount", std::to_string(count).c_str(), g_statePath.string().c_str());
}

std::uint16_t FindZoneId(const std::string& name) {
    const std::uint32_t maxId = ZoneList::MaxId();
    for (std::uint32_t id = 1; id <= maxId && id <= 0xFFFFu; ++id) {
        const char* zone = ZoneList::Name(id);
        if (zone && zone[0] && _stricmp(zone, name.c_str()) == 0) return static_cast<std::uint16_t>(id);
    }
    return 0;
}

// ---- new models: the framework archive (built on its own thread), new object names -----------------------------

HANDLE g_buildDoneEvent = nullptr;
std::atomic_bool g_buildDone{true};
std::atomic_bool g_buildStarted{false};
std::mutex g_buildMutex;
std::vector<OutfitArchive::BuiltObject> g_builtObjects;   // objects whose .drm the framework archive has
std::vector<OutfitArchive::RenamedResource> g_renamed;   // supplied resources given new IDs (read under g_buildMutex)
std::vector<OutfitArchive::RenamedResource> g_copied;    // the game's resources copied into outfits with new IDs
std::vector<OutfitArchive::BuiltPicture> g_builtPictures;   // custom campsite pictures (read under g_buildMutex)
// Their textures larger than the game's 128x256 (twice its resolution): ID -> size; the menu's image is given the
// game's size so it is drawn in the same place, the texture's pixels scaled into it (HookCustomImageLoad).
std::mutex g_largePictureMutex;
std::vector<std::pair<std::uint32_t, std::pair<std::uint16_t, std::uint16_t>>> g_largePictures;
std::deque<std::string> g_picturePaths;                     // the outfit entries point at these
bool g_buildOk = false;

using FindFirstFileExAFn = HANDLE(WINAPI*)(LPCSTR, FINDEX_INFO_LEVELS, LPVOID, FINDEX_SEARCH_OPS, LPVOID, DWORD);
FindFirstFileExAFn g_origFindFirstFileExA = nullptr;
std::atomic_bool g_archiveScanWaited{false};

// The game finds its archives with FindFirstFileExA("*.nfo"): it waits here until the framework archive is written.
// (v0.9.606) when the game's first archive scan came, from the process start (whether the build was on its way)
long long MsSinceProcessStart() {
    FILETIME created{}, exited{}, kernel{}, user{}, now{};
    if (!GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user)) return -1;
    GetSystemTimeAsFileTime(&now);
    const auto u64 = [](const FILETIME& f) { return (ULONGLONG(f.dwHighDateTime) << 32) | f.dwLowDateTime; };
    return static_cast<long long>((u64(now) - u64(created)) / 10000ull);
}
std::atomic_bool g_archiveScanLogged{false};

HANDLE WINAPI HookFindFirstFileExA(LPCSTR name, FINDEX_INFO_LEVELS level, LPVOID data, FINDEX_SEARCH_OPS op,
                                   LPVOID filter, DWORD flags) {
    if (name && !g_archiveScanLogged.load(std::memory_order_relaxed)) {
        const std::size_t length = std::strlen(name);
        if (length >= 5 && _stricmp(name + length - 5, "*.nfo") == 0 && !g_archiveScanLogged.exchange(true))
            Log::Writef("CustomOutfits: the game's first archive scan at %lld ms from the process start (the custom outfit "
                        "archive %s)", MsSinceProcessStart(),
                        g_buildDone.load(std::memory_order_acquire) ? "ready" : "still being made: the scan waits");
    }
    if (name && !g_buildDone.load(std::memory_order_acquire)) {
        const std::size_t length = std::strlen(name);
        if (length >= 5 && _stricmp(name + length - 5, "*.nfo") == 0 && g_buildDoneEvent) {
            const DWORD start = GetTickCount();
            // (up to 10 minutes: past the wait the game mounts the archive built before, and a new one is only used
            // at the next start; 0.9.402 and earlier waited 2 minutes, and 107 outfits took 163 s to build)
            const DWORD wait = WaitForSingleObject(g_buildDoneEvent, 600000);
            g_archiveScanWaited.store(true);
            const DWORD waited = GetTickCount() - start;
            if (waited >= 1000 || wait != WAIT_OBJECT_0)
                Log::Writef("CustomOutfits: the game's archive scan waited %lu ms for the custom outfit archive%s",
                            waited, wait == WAIT_OBJECT_0 ? "" : " (timed out)");
            else
                Log::Detailf("CustomOutfits: the game's archive scan waited %lu ms for the custom outfit archive", waited);
        }
    }
    return g_origFindFirstFileExA(name, level, data, op, filter, flags);
}

void BuildArchiveThread(std::vector<OutfitArchive::OutfitSpec> specs, std::vector<OutfitArchive::RawFile> files) {
    const DWORD start = GetTickCount();
    const auto result = OutfitArchive::Build(GameRoot(), specs, KnownZoneCount(), [](const std::string& line) {
        Log::Writef("CustomOutfits: %s", line.c_str());
    }, files);
    {
        std::lock_guard<std::mutex> lock(g_buildMutex);
        g_builtObjects = result.objects;
        g_renamed = result.renamed;
        g_copied = result.copied;
        g_builtPictures = result.pictures;
        std::lock_guard<std::mutex> large(g_largePictureMutex);
        g_largePictures.clear();
        for (const auto& p : result.pictures)
            if (p.texture && p.width > 128) g_largePictures.push_back({p.texture, {p.width, p.height}});
        g_buildOk = result.ok;
    }
    Log::Writef("CustomOutfits: %s%s (%lu ms)", result.ok ? "" : "archive not built: ", result.message.c_str(),
                GetTickCount() - start);
    g_buildDone.store(true, std::memory_order_release);
    if (g_buildDoneEvent) SetEvent(g_buildDoneEvent);
}

// New object names, appended to the game's zone name table (IDs after the highest one).
std::deque<std::string> g_zoneNameStore;   // the game's name -> ID map keeps these pointers
const char** g_zoneNames = nullptr;
const char** g_gameZoneNames = nullptr;
int g_gameZoneCount = 0;
bool g_zoneNamesRegistered = false;
using ZoneNameMapInsertFn = std::uint32_t*(__fastcall*)(void*, const void*);
using ClearZoneNamesFn = void(__fastcall*)(void*);
ClearZoneNamesFn g_origClearZoneNames = nullptr;
bool g_zoneRegistrationAvailable = false;

void __fastcall HookClearZoneNames(void* manager) {
    // The game frees its own arrays: give them back first (the names are registered again afterwards).
    auto* bytes = static_cast<std::uint8_t*>(manager);
    if (g_zoneNames && *reinterpret_cast<const char***>(bytes + 0x148) == g_zoneNames) {
        *reinterpret_cast<const char***>(bytes + 0x148) = g_gameZoneNames;
        *reinterpret_cast<int*>(bytes + 0x140) = g_gameZoneCount;
        g_zoneNamesRegistered = false;
    }
    g_origClearZoneNames(manager);
    ZoneList::Refresh();
}

std::uint16_t FindZoneId(const std::string& name);

// Each new object goes to the zone ID the archive builder gave it (the game's highest + 1, + 2, ...: its zone
// resource in the .drm has that ID), padding unused IDs with "" like the game's own table.
bool RegisterZoneNames(const std::vector<OutfitArchive::BuiltObject>& objects) {
    auto* manager = *reinterpret_cast<std::uint8_t**>(GameBuild::Base() + kRvaObjectZoneManagerPtr);
    if (!manager) return false;
    const int count = *reinterpret_cast<const int*>(manager + 0x140);
    const char** gameNames = *reinterpret_cast<const char***>(manager + 0x148);
    if (count <= 0 || !gameNames) return false;
    if (gameNames != g_zoneNames) SaveZoneCount(static_cast<std::uint32_t>(count));   // the game's own table

    std::vector<const OutfitArchive::BuiltObject*> missing;
    int total = count;
    for (const auto& object : objects) {
        const int id = static_cast<int>(object.zoneId);
        if (id <= 0 || id > 0xFFFF) continue;
        if (id <= count) {
            const char* existing = gameNames[id - 1];
            if (!existing || _stricmp(existing, object.name.c_str()) != 0)
                Log::Writef("CustomOutfits: object %s cannot be registered: its zone ID %d is the game's %s (the game's "
                            "object list is longer than the archive expected; restart the game: it is rebuilt with higher IDs)",
                            object.name.c_str(), id, existing && existing[0] ? existing : "(unused)");
            continue;
        }
        missing.push_back(&object);
        total = std::max(total, id);
    }
    if (missing.empty()) return true;

    static const char kUnused[] = "";
    auto* table = new const char*[static_cast<std::size_t>(total)];
    std::memcpy(table, gameNames, sizeof(const char*) * static_cast<std::size_t>(count));
    for (int i = count; i < total; ++i) table[i] = kUnused;
    for (const auto* object : missing) {
        g_zoneNameStore.push_back(object->name);
        table[object->zoneId - 1] = g_zoneNameStore.back().c_str();
    }
    if (g_zoneNames != gameNames) {
        g_gameZoneNames = gameNames;
        g_gameZoneCount = count;
    }
    g_zoneNames = table;   // the previous copy (if any) is leaked on purpose: other threads may still read it
    *reinterpret_cast<const char***>(manager + 0x148) = table;
    *reinterpret_cast<int*>(manager + 0x140) = total;
    ZoneList::Refresh();   // its copy of the table pointer and count (FindZoneId reads through it)
    const auto insert = reinterpret_cast<ZoneNameMapInsertFn>(GameBuild::Base() + kRvaZoneNameMapInsert);
    for (const auto* object : missing) {
        const char* hashString = table[object->zoneId - 1];   // cdc::HashString: the string pointer
        if (std::uint32_t* slot = insert(manager + 0x158, &hashString)) *slot = object->zoneId;
        Log::Detailf("CustomOutfits: object %s registered as zone %u", hashString, object->zoneId);
    }
    return true;
}

bool ResolveConfigs() {
    if (!ZoneList::Ready() || !g_buildDone.load(std::memory_order_acquire)) return false;
    if (!g_zoneNamesRegistered) {
        std::vector<OutfitArchive::BuiltObject> built;
        {
            std::lock_guard<std::mutex> lock(g_buildMutex);
            built = g_builtObjects;
        }
        if (!built.empty() && g_zoneRegistrationAvailable && !RegisterZoneNames(built)) return false;
        g_zoneNamesRegistered = true;
        for (auto& config : g_configs) config.resolved = false;   // names may be new now
    }
    for (auto& config : g_configs) {
        if (config.resolved) continue;
        config.baseObject = FindZoneId(config.baseName);
        config.object = FindZoneId(config.objectName);
        config.resolved = true;
        if (!config.baseObject || !config.object)
            Log::Writef("CustomOutfits: %s: %s%s%s is not an object the game knows (a new Object= needs the outfit's "
                        "resource files in its folder)", config.key.c_str(), !config.baseObject ? "Base=" : "Object=",
                        !config.baseObject ? config.baseName.c_str() : config.objectName.c_str(),
                        !config.baseObject && !config.object ? " (and Object=)" : "");
    }
    return true;
}

bool CopyGuarded(void* destination, const void* source, std::size_t size) noexcept {
    __try {
        std::memcpy(destination, source, size);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

std::uint16_t ReadU16(const std::uint8_t* p, std::ptrdiff_t offset) {
    std::uint16_t value = 0;
    std::memcpy(&value, p + offset, sizeof(value));
    return value;
}
void WriteU16(std::uint8_t* p, std::ptrdiff_t offset, std::uint16_t value) { std::memcpy(p + offset, &value, 2); }
void WritePtr(std::uint8_t* p, std::ptrdiff_t offset, const void* value) {
    std::memcpy(p + offset, &value, sizeof(value));
}

// A built campsite picture of the config (if any) into an outfit entry.
void WritePicture(OutfitConfig& config, std::uint8_t* entry) {
    std::lock_guard<std::mutex> lock(g_buildMutex);
    for (const auto& picture : g_builtPictures)
        if (picture.key == config.key) {
            const std::uint32_t imageRes = picture.imageRes;
            std::memcpy(entry + kOutfitImageRes, &imageRes, sizeof(imageRes));
            auto found = std::find(g_picturePaths.begin(), g_picturePaths.end(), picture.path);
            if (found == g_picturePaths.end()) found = g_picturePaths.insert(g_picturePaths.end(), picture.path);
            WritePtr(entry, kOutfitImagePath, found->c_str());
            if (!config.pictureReported)
                Log::Detailf("CustomOutfits: %s: its own picture (%s, data %u)", config.key.c_str(), picture.path.c_str(),
                            imageRes);
            config.pictureReported = true;
        }
}

// Outfit list: the game's entries (retail outfits a mod replaces wear its object and picture), then one copy of the
// base outfit per custom outfit.
void ApplyOutfits(std::uint8_t* crafting) {
    std::uint8_t* list = nullptr;
    unsigned int count = 0;
    if (!CopyGuarded(&list, crafting + kCraftingOutfitsList, sizeof(list)) ||
        !CopyGuarded(&count, crafting + kCraftingNumOutfits, sizeof(count)) || !list || !count || count > 256)
        return;
    if (list == g_outfitList && count == g_outfitCount) return;   // ours, unchanged
    if (list == g_outfitList) count = g_gameOutfitCount;           // count reset under our list: the game's part
    // An outfit is worn through the character profile of its object (the campsite finds the profile by the object):
    // the outfits wait for the profiles, and one that cannot have a profile is left out (its base has none; or, without
    // the equip hook, its object got no slot).
    if (!g_profilesApplied) return;
    std::vector<bool> wearable(g_configs.size());
    {
        std::lock_guard<std::mutex> lock(g_profileMutex);
        for (std::size_t i = 0; i < g_configs.size(); ++i)
            wearable[i] = g_configs[i].profileBase >= 0 && (g_configs[i].profileIndex >= 0 || g_slotsOnEquip);
    }

    std::vector<AppliedOutfit> applied;
    const unsigned int maxOutfits = MaxOutfits();
    const unsigned int room = count < maxOutfits ? maxOutfits - count : 0;
    std::vector<std::size_t> usable;
    std::size_t left = 0;
    for (std::size_t i = 0; i < g_configs.size(); ++i)
        if (g_configs[i].baseObject && g_configs[i].object && g_configs[i].replaces.empty() && wearable[i]) {
            if (usable.size() < room) usable.push_back(i);
            else ++left;
        }
    if (left)
        Log::WriteOncef("CustomOutfits: %zu custom outfit(s) left out: the campsite's outfit list holds %u outfits%s", left,
                        maxOutfits, maxOutfits ? "" : " (its rows could not be extended past the game's 32)");

    const unsigned int total = count + static_cast<unsigned int>(usable.size());
    auto buffer = std::make_unique<std::uint8_t[]>(static_cast<std::size_t>(total) * kOutfitStride);
    if (!CopyGuarded(buffer.get(), list, static_cast<std::size_t>(count) * kOutfitStride)) return;

    // Mods' overrides of retail outfits (by m_debugName): text, and "unlock": "always".
    std::vector<unsigned int> forced;
    int overridden = 0;
    if (!g_overrides.empty())
        for (unsigned int i = 0; i < count; ++i) {
            std::uint8_t* entry = buffer.get() + std::size_t(i) * kOutfitStride;
            const char* debugName = nullptr;
            std::memcpy(&debugName, entry + kOutfitDebugName, sizeof(debugName));
            char name[64]{};
            for (std::size_t k = 0; debugName && k + 1 < sizeof(name); ++k)
                if (!CopyGuarded(&name[k], debugName + k, 1) || !name[k]) break;
            for (auto& o : g_overrides) {
                if (_stricmp(o.outfit.c_str(), name) != 0) continue;
                if (o.nameStringId >= 0) WriteU16(entry, kOutfitName, static_cast<std::uint16_t>(o.nameStringId));
                if (o.descriptionStringId >= 0)
                    WriteU16(entry, kOutfitDescription, static_cast<std::uint16_t>(o.descriptionStringId));
                if (o.alwaysUnlocked) forced.push_back(i);
                if (!o.reported)
                    Log::Detailf("CustomOutfits: mod %s changes outfit %u (%s)%s", o.mod.c_str(), i, name,
                                o.alwaysUnlocked ? ": always available" : "");
                o.reported = true;
                ++overridden;
            }
        }

    // Mods' replacements of retail outfits ("replaces": the m_debugName): the entry wears the mod's object (a new
    // model, or the retail object itself), shows its picture and text; its index, unlock and DLC rules stay.
    std::vector<ReplacedOutfit> replaced;
    for (std::size_t c = 0; c < g_configs.size(); ++c) {
        auto& config = g_configs[c];
        if (config.replaces.empty() || !config.object || !wearable[c]) continue;
        bool found = false;
        for (unsigned int i = 0; i < count && !found; ++i) {
            std::uint8_t* entry = buffer.get() + std::size_t(i) * kOutfitStride;
            const char* debugName = nullptr;
            std::memcpy(&debugName, entry + kOutfitDebugName, sizeof(debugName));
            char name[64]{};
            for (std::size_t k = 0; debugName && k + 1 < sizeof(name); ++k)
                if (!CopyGuarded(&name[k], debugName + k, 1) || !name[k]) break;
            if (_stricmp(config.replaces.c_str(), name) != 0) continue;
            found = true;
            WriteU16(entry, kOutfitPlayerObject, config.object);
            if (config.nameStringId >= 0) WriteU16(entry, kOutfitName, static_cast<std::uint16_t>(config.nameStringId));
            if (config.descriptionStringId >= 0)
                WriteU16(entry, kOutfitDescription, static_cast<std::uint16_t>(config.descriptionStringId));
            WritePicture(config, entry);
            replaced.push_back({c, i});
        }
        if (!found && !config.reported)
            Log::Writef("CustomOutfits: %s: the game has no outfit %s to replace", config.key.c_str(), config.replaces.c_str());
        config.reported = true;
    }

    unsigned int next = count;
    for (const std::size_t c : usable) {
        auto& config = g_configs[c];
        unsigned int baseIndex = count;
        for (unsigned int i = 0; i < count; ++i)
            if (ReadU16(buffer.get() + std::size_t(i) * kOutfitStride, kOutfitPlayerObject) == config.baseObject) {
                baseIndex = i;
                break;
            }
        if (baseIndex == count) {
            if (!config.reported)
                Log::Writef("CustomOutfits: %s: no campsite outfit wears %s (Base=)", config.key.c_str(),
                            config.baseName.c_str());
            config.reported = true;
            continue;
        }
        std::uint8_t* entry = buffer.get() + std::size_t(next) * kOutfitStride;
        std::memcpy(entry, buffer.get() + std::size_t(baseIndex) * kOutfitStride, kOutfitStride);
        WriteU16(entry, kOutfitPlayerObject, config.object);
        if (config.nameStringId >= 0) WriteU16(entry, kOutfitName, static_cast<std::uint16_t>(config.nameStringId));
        if (config.descriptionStringId >= 0)
            WriteU16(entry, kOutfitDescription, static_cast<std::uint16_t>(config.descriptionStringId));
        WritePtr(entry, kOutfitDebugName, config.key.c_str());
        WritePicture(config, entry);
        applied.push_back({c, next, baseIndex});
        ++next;
    }
    // Mods' removed retail outfits: their indexes (the menu leaves them out: HookHasOutfit, HookIsOutfitCraftable).
    if (!g_removed.empty()) {
        std::vector<unsigned int> removed;
        for (unsigned int i = 0; i < count; ++i) {
            const std::uint8_t* entry = buffer.get() + std::size_t(i) * kOutfitStride;
            const char* debugName = nullptr;
            std::memcpy(&debugName, entry + kOutfitDebugName, sizeof(debugName));
            char name[64]{};
            for (std::size_t k = 0; debugName && k + 1 < sizeof(name); ++k)
                if (!CopyGuarded(&name[k], debugName + k, 1) || !name[k]) break;
            for (auto& r : g_removed) {
                if (_stricmp(r.name.c_str(), name) != 0) continue;
                removed.push_back(i);
                if (!r.reported) Log::Detailf("CustomOutfits: mod %s removes outfit %u (%s) from the campsite", r.mod.c_str(), i, name);
                r.reported = true;
            }
        }
        std::lock_guard<std::mutex> lock(g_appliedMutex);
        g_removedIndexes = std::move(removed);
    }
    if (applied.empty() && replaced.empty() && !overridden && g_orders.empty()) return;

    // Mods' campsite orders: every entry gets its own sort order (the menu sorts by it), in the game's order (by
    // sort order, then list index: retail first, custom outfits after) with each mod's named outfits moved into the
    // places they had, in the mod's order.
    if (!g_orders.empty()) {
        auto nameOf = [&](unsigned int i) -> std::string {
            for (const auto& a : applied)
                if (a.listIndex == i) return g_configs[a.config].key;
            const std::uint8_t* entry = buffer.get() + std::size_t(i) * kOutfitStride;
            const char* debugName = nullptr;
            std::memcpy(&debugName, entry + kOutfitDebugName, sizeof(debugName));
            char name[64]{};
            for (std::size_t k = 0; debugName && k + 1 < sizeof(name); ++k)
                if (!CopyGuarded(&name[k], debugName + k, 1) || !name[k]) break;
            return name;
        };
        std::vector<unsigned int> sequence(next);
        std::vector<std::string> names(next);
        for (unsigned int i = 0; i < next; ++i) sequence[i] = i, names[i] = nameOf(i);
        std::stable_sort(sequence.begin(), sequence.end(), [&](unsigned int a, unsigned int b) {
            return ReadU16(buffer.get() + std::size_t(a) * kOutfitStride, kOutfitSortOrder) <
                   ReadU16(buffer.get() + std::size_t(b) * kOutfitStride, kOutfitSortOrder);
        });
        for (auto& order : g_orders) {
            std::vector<unsigned int> wanted;   // list indexes, in the mod's order
            for (const auto& entry : order.entries)
                for (unsigned int i = 0; i < next; ++i)
                    if (_stricmp(names[i].c_str(), entry.c_str()) == 0 &&
                        std::find(wanted.begin(), wanted.end(), i) == wanted.end()) {
                        wanted.push_back(i);
                        break;
                    }
            std::vector<std::size_t> places;    // where those are in the sequence now
            for (std::size_t p = 0; p < sequence.size(); ++p)
                if (std::find(wanted.begin(), wanted.end(), sequence[p]) != wanted.end()) places.push_back(p);
            for (std::size_t k = 0; k < places.size() && k < wanted.size(); ++k) sequence[places[k]] = wanted[k];
            if (!order.reported)
                Log::Detailf("CustomOutfits: mod %s orders %zu of the campsite's outfits", order.mod.c_str(), wanted.size());
            order.reported = true;
        }
        for (std::size_t p = 0; p < sequence.size(); ++p)
            WriteU16(buffer.get() + std::size_t(sequence[p]) * kOutfitStride, kOutfitSortOrder, static_cast<std::uint16_t>(p));
    }

    std::uint8_t* newList = buffer.get();
    g_tableBuffers.push_back(std::move(buffer));
    g_gameOutfitCount = count;
    g_outfitList = newList;
    g_outfitCount = next;
    {
        std::lock_guard<std::mutex> lock(g_appliedMutex);
        g_applied = applied;
        g_replaced = replaced;
        g_forcedUnlocked = forced;
    }
    g_activeCount.store(static_cast<int>(applied.size()));
    // List first, then the larger count: a reader in between still sees every index it can reach.
    std::memcpy(crafting + kCraftingOutfitsList, &newList, sizeof(newList));
    std::memcpy(crafting + kCraftingNumOutfits, &next, sizeof(next));
    Log::Detailf("CustomOutfits: outfit list %u -> %u entries", count, next);
    for (const auto& r : replaced)
        Log::Detailf("CustomOutfits:   [%u] %s replaced by %s: wears %s (zone %u)", r.listIndex,
                    g_configs[r.config].replaces.c_str(), g_configs[r.config].key.c_str(),
                    g_configs[r.config].objectName.c_str(), g_configs[r.config].object);
    for (const auto& a : applied)
        Log::Detailf("CustomOutfits:   [%u] %s: wears %s (zone %u), copied from outfit %u (%s)", a.listIndex,
                    g_configs[a.config].key.c_str(), g_configs[a.config].objectName.c_str(), g_configs[a.config].object,
                    a.baseIndex, g_configs[a.config].baseName.c_str());
}

// Character profiles: the player is spawned from the profile of the outfit's object (dtp::CharacterProfileItem: the
// object to spawn, the menu's object, the inventory, the player state...); the campsite equips an outfit only when a
// profile has its object, and from then on the player is that profile's number (a byte in the player data, the saves,
// the last outfit worn). A new object (a custom outfit's model) gets a copy of its base outfit's profile in one of the
// slots past the game's profiles (g_maxProfiles - the game's 90: 165).
//
// v0.9.402: a slot belongs to an object, not for good: the slots are given to the objects when the profiles are made
// (each object to the slot it had before, kept in _state.ini [ProfileSlots] "<object>=<slot> <last used>"; then the
// free ones), and when there are more objects than slots, an object without one gets one when its outfit is equipped
// (UiMenuStateCampsiteGear::EquipSelectedWeapon's hook, before the game looks for its profile): the slot used longest
// ago, never the worn object's. A save made in an outfit whose slot went to another object since comes back in that
// one (only past 165 objects worn).

std::string LowerObject(const std::string& name) {
    std::string out = name;
    for (char& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

void SaveSlot(std::size_t k) {   // (under g_profileMutex)
    if (g_slots[k].object.empty()) return;
    const std::string value = std::to_string(k) + " " + std::to_string(g_slots[k].used);
    WritePrivateProfileStringA("ProfileSlots", g_slots[k].object.c_str(), value.c_str(), g_statePath.string().c_str());
}
void ForgetSlot(const std::string& object) {
    WritePrivateProfileStringA("ProfileSlots", object.c_str(), nullptr, g_statePath.string().c_str());
}

// The slots as the state file has them (once): object -> slot, last used.
void LoadSlots(std::size_t pool) {
    g_slots.assign(pool, ProfileSlot{});
    std::vector<char> section(1 << 16);
    const DWORD n = GetPrivateProfileSectionA("ProfileSlots", section.data(), static_cast<DWORD>(section.size()),
                                              g_statePath.string().c_str());
    for (const char* line = section.data(); line < section.data() + n && *line; line += std::strlen(line) + 1) {
        const char* eq = std::strchr(line, '=');
        if (!eq) continue;
        const std::string object = LowerObject(std::string(line, eq));
        unsigned long long slot = 0, used = 0;
        if (std::sscanf(eq + 1, "%llu %llu", &slot, &used) < 1 || slot >= pool || object.empty()) continue;
        if (!g_slots[slot].object.empty() && g_slots[slot].used >= used) continue;   // (two on one slot: the later)
        g_slots[slot] = {object, used};
        g_slotClock = std::max<std::uint64_t>(g_slotClock, used);
    }
}

// The slot to give an object: a free one, else the one used longest ago among those of objects no outfit wears now,
// else (when allowed) among the others but the protected ones. -1: none.
int PickSlot(const std::vector<std::string>& present, bool takePresent, const std::vector<std::string>& protect) {
    for (std::size_t k = 0; k < g_slots.size(); ++k)
        if (g_slots[k].object.empty()) return static_cast<int>(k);
    for (int pass = 0; pass < (takePresent ? 2 : 1); ++pass) {
        int pick = -1;
        for (std::size_t k = 0; k < g_slots.size(); ++k) {
            const auto& o = g_slots[k].object;
            const bool isPresent = std::find(present.begin(), present.end(), o) != present.end();
            if (isPresent != (pass == 1) || std::find(protect.begin(), protect.end(), o) != protect.end()) continue;
            if (pick < 0 || g_slots[k].used < g_slots[static_cast<std::size_t>(pick)].used) pick = static_cast<int>(k);
        }
        if (pick >= 0) return pick;
    }
    return -1;
}

// The first config wearing an object that needs a slot (-1: none).
int SlotConfigFor(const std::string& object) {
    for (std::size_t i = 0; i < g_configs.size(); ++i)
        if (g_configs[i].profileBase >= 0 && g_configs[i].needsSlot && LowerObject(g_configs[i].objectName) == object)
            return static_cast<int>(i);
    return -1;
}

// A slot's table entry: a copy of the base outfit's profile wearing the object, or (no object of this session) a copy
// of the game's first profile wearing nothing (object 0: a save on it gets the level's profile).
void FillSlot(std::uint8_t* table, std::size_t k) {
    std::uint8_t* entry = table + std::size_t(g_gameProfileCount + k) * kProfileStride;
    const int c = g_slots[k].object.empty() ? -1 : SlotConfigFor(g_slots[k].object);
    if (c < 0) {
        static const char kFree[] = "rottrframework_free_profile";
        std::memcpy(entry, table, kProfileStride);
        WriteU16(entry, kProfileObjectId, 0);
        WriteU16(entry, kProfileMenuObjectId, 0);
        WritePtr(entry, kProfileDebugName, kFree);
        return;
    }
    const OutfitConfig& config = g_configs[static_cast<std::size_t>(c)];
    std::memcpy(entry, table + std::size_t(config.profileBase) * kProfileStride, kProfileStride);
    const std::uint16_t baseMenuObject = ReadU16(entry, kProfileMenuObjectId);
    WriteU16(entry, kProfileObjectId, config.object);
    WritePtr(entry, kProfileObjectName, config.objectName.c_str());
    WritePtr(entry, kProfileDebugName, config.key.c_str());
    if (baseMenuObject == config.baseObject) {
        WriteU16(entry, kProfileMenuObjectId, config.object);
        WritePtr(entry, kProfileMenuObjectName, config.objectName.c_str());
    }
}

// The configs' profiles from the slots (under g_profileMutex).
void UpdateSlotIndexes() {
    for (auto& config : g_configs) {
        if (!config.needsSlot) continue;
        config.profileIndex = -1;
        const std::string object = LowerObject(config.objectName);
        for (std::size_t k = 0; k < g_slots.size(); ++k)
            if (g_slots[k].object == object) config.profileIndex = static_cast<int>(g_gameProfileCount + k);
    }
}

// The profile count: the game's and the slots up to the last one in use (under g_profileMutex).
void UpdateProfileCount() {
    std::size_t used = 0;
    for (std::size_t k = 0; k < g_slots.size(); ++k)
        if (!g_slots[k].object.empty()) used = k + 1;
    const unsigned int total = g_gameProfileCount + static_cast<unsigned int>(used);
    if (total == g_profileCount || !g_profileUi) return;
    g_profileCount = total;
    const std::uint8_t count = static_cast<std::uint8_t>(total);
    std::memcpy(g_profileUi + kUiProfileCount, &count, 1);
}

void ApplyProfiles(std::uint8_t* ui) {
    std::uint8_t* list = nullptr;
    std::uint8_t count = 0;
    if (!CopyGuarded(&list, ui + kUiProfiles, sizeof(list)) || !CopyGuarded(&count, ui + kUiProfileCount, 1) ||
        !list || !count)
        return;
    if (list == g_profileList) return;   // ours (its count is ours too)

    std::lock_guard<std::mutex> lock(g_profileMutex);
    auto findProfile = [&](const std::uint8_t* table, unsigned int n, std::uint16_t object) -> int {
        for (unsigned int i = 0; i < n; ++i)
            if (ReadU16(table + std::size_t(i) * kProfileStride, kProfileObjectId) == object) return static_cast<int>(i);
        return -1;
    };
    const unsigned int capacity = std::max<unsigned int>(g_maxProfiles, count);
    auto copy = std::make_unique<std::uint8_t[]>(std::size_t(capacity) * kProfileStride);
    if (!CopyGuarded(copy.get(), list, std::size_t(count) * kProfileStride)) return;
    g_gameProfileCount = count;

    // each config: its object's own profile, else its base's (copied into a slot)
    std::vector<std::string> wanted;   // the objects needing a slot, in the configs' order
    for (auto& config : g_configs) {
        config.profileIndex = -1;
        config.profileBase = -1;
        config.needsSlot = false;
        if (!config.baseObject || !config.object) continue;
        if (const int own = findProfile(copy.get(), count, config.object); own >= 0) {   // it has a profile already
            config.profileIndex = own;
            config.profileBase = own;
            continue;
        }
        const int base = findProfile(copy.get(), count, config.baseObject);
        if (base < 0) {
            Log::Writef("CustomOutfits: %s: the base object %s has no character profile", config.key.c_str(),
                        config.baseName.c_str());
            continue;
        }
        config.profileBase = base;
        config.needsSlot = true;
        const std::string object = LowerObject(config.objectName);
        if (std::find(wanted.begin(), wanted.end(), object) == wanted.end()) wanted.push_back(object);
    }

    // the slots: kept from before, then the free ones, then those of objects no outfit wears now (used longest ago)
    const std::size_t pool = capacity > count ? capacity - count : 0;
    if (!g_slotsLoaded || g_slots.size() != pool) {
        LoadSlots(pool);
        g_slotsLoaded = true;
    }
    std::vector<std::string> waiting;
    for (const auto& object : wanted) {
        bool has = false;
        for (const auto& slot : g_slots) has = has || slot.object == object;
        if (!has) waiting.push_back(object);
    }
    std::size_t given = 0;
    for (const auto& object : waiting) {
        const int pick = PickSlot(wanted, false, {});
        if (pick < 0) break;   // every slot is an object of this session's: the rest get one when equipped
        auto& slot = g_slots[static_cast<std::size_t>(pick)];
        if (!slot.object.empty()) ForgetSlot(slot.object);
        slot = {object, ++g_slotClock};
        SaveSlot(static_cast<std::size_t>(pick));
        ++given;
    }
    for (std::size_t k = 0; k < g_slots.size(); ++k) FillSlot(copy.get(), k);
    UpdateSlotIndexes();

    std::size_t slotted = 0, unslotted = 0;
    for (const auto& object : wanted) {
        bool has = false;
        for (const auto& slot : g_slots) has = has || slot.object == object;
        (has ? slotted : unslotted)++;
    }
    Log::Detailf("CustomOutfits: character profiles: the game's %u, %zu slot(s) for %zu new object(s): %zu with one "
                 "(%zu new)%s", count, pool, wanted.size(), slotted, given,
                 unslotted ? (g_slotsOnEquip ? ", the others get one when equipped" : ", the others cannot be worn") : "");
    if (unslotted && !g_slotsOnEquip)
        Log::WriteOncef("CustomOutfits: %zu custom outfit model(s) left out: the character profiles hold %zu of them",
                        unslotted, pool);

    std::uint8_t* newList = copy.get();
    g_tableBuffers.push_back(std::move(copy));
    g_profileList = newList;
    g_profileUi = ui;
    g_profileCount = count;
    std::memcpy(ui + kUiProfiles, &newList, sizeof(newList));
    UpdateProfileCount();
    g_profilesApplied = true;
}

// An object's slot when its outfit is equipped (the game looks for the profile right after): kept (its use noted), or
// given: a free slot, else the one used longest ago whose object is not worn (protect: the objects worn, being
// changed to or from). false: none could be given.
bool GiveSlot(std::uint16_t objectId, const std::vector<std::uint16_t>& protect) {
    std::lock_guard<std::mutex> lock(g_profileMutex);
    if (!g_profileList || g_slots.empty()) return false;
    int c = -1;
    for (std::size_t i = 0; i < g_configs.size() && c < 0; ++i)
        if (g_configs[i].object == objectId && g_configs[i].needsSlot) c = static_cast<int>(i);
    if (c < 0) return true;   // (the game's object, or one with its own profile)
    const std::string object = LowerObject(g_configs[static_cast<std::size_t>(c)].objectName);
    for (std::size_t k = 0; k < g_slots.size(); ++k)
        if (g_slots[k].object == object) {
            g_slots[k].used = ++g_slotClock;
            SaveSlot(k);
            return true;
        }
    std::vector<std::string> present, guarded;
    for (const auto& config : g_configs) {
        if (!config.needsSlot) continue;
        const std::string o = LowerObject(config.objectName);
        if (std::find(present.begin(), present.end(), o) == present.end()) present.push_back(o);
        if (std::find(protect.begin(), protect.end(), config.object) != protect.end()) guarded.push_back(o);
    }
    const int pick = PickSlot(present, true, guarded);
    if (pick < 0) {
        Log::Writef("CustomOutfits: %s: no character profile slot could be freed for it", object.c_str());
        return false;
    }
    auto& slot = g_slots[static_cast<std::size_t>(pick)];
    const std::string was = slot.object;
    if (!was.empty()) ForgetSlot(was);
    slot = {object, ++g_slotClock};
    SaveSlot(static_cast<std::size_t>(pick));
    FillSlot(g_profileList, static_cast<std::size_t>(pick));
    UpdateSlotIndexes();
    UpdateProfileCount();
    Log::Detailf("CustomOutfits: character profile %u now %s%s%s", g_gameProfileCount + static_cast<unsigned int>(pick),
                 object.c_str(), was.empty() ? "" : " (was ", was.empty() ? "" : (was + ")").c_str());
    return true;
}

int AppliedConfigForIndex(unsigned int index, unsigned int* baseIndex = nullptr) {
    std::lock_guard<std::mutex> lock(g_appliedMutex);
    for (const auto& a : g_applied)
        if (a.listIndex == index) {
            if (baseIndex) *baseIndex = a.baseIndex;
            return static_cast<int>(a.config);
        }
    return -1;
}

const char* PartString(int id);

const char* __fastcall HookLocalstrGet(int id, bool* found) {
    if (g_partStringTop >= 0 && id <= g_partStringTop && id > g_partStringTop - kMaxParts * 2) {
        if (const char* text = PartString(id)) {
            if (found) *found = true;
            return text;
        }
    }
    if (id >= g_stringIdMin && id <= kStringIdTop) {
        const char* text = g_strings[static_cast<std::size_t>(kStringIdTop - id)];
        if (text) {
            if (found) *found = true;
            return text;
        }
    }
    return g_origLocalstrGet(id, found);
}

// PlayerProfile's last outfit worn is the character profile the player wears when the campsite's Outfits menu closes
// (UiMenuStateCampsiteGear::OnExit stores Player::GetCharacterProfileIndex, a signed char, as an int; the replay modes
// spawn the player with it: ReplayInfo::GetActiveOutfitIndex): kept as it is. The framework remembers the custom
// outfit wearing that profile's object (the one remembered already when several wear it).
void __fastcall HookSetLastOutfitWorn(void* profile, int index) {
    const int profileIndex = index == -1 ? -1 : (index & 0xFF);
    int config = -1;
    if (profileIndex >= 0) {
        std::lock_guard<std::mutex> lock(g_profileMutex);
        const int last = g_lastWornConfig.load();
        if (last >= 0 && static_cast<std::size_t>(last) < g_configs.size() && g_configs[last].profileIndex == profileIndex)
            config = last;
        for (std::size_t i = 0; config < 0 && i < g_configs.size(); ++i)
            if (g_configs[i].profileIndex == profileIndex) config = static_cast<int>(i);
    }
    SaveLastWorn(config);
    g_origSetLastOutfitWorn(profile, index);
}

// Custom outfits are always available at the campsite, whatever their base outfit needs (a trait, a DLC, crafting):
// the profile's unlock bits (64 outfits), Crafting::HasOutfit (the base's unlock trait, or its DLC for DLC outfits)
// and Crafting::HasDLCForOutfit answer yes for them (v0.9.455: and every retail outfit, AlwaysAvailable).
bool ForcedUnlocked(unsigned int index) {
    std::lock_guard<std::mutex> lock(g_appliedMutex);
    return std::find(g_forcedUnlocked.begin(), g_forcedUnlocked.end(), index) != g_forcedUnlocked.end();
}

using HasOutfitFn = bool(__fastcall*)(void*, unsigned int);
using HasDlcForOutfitFn = bool(__fastcall*)(unsigned int);
HasOutfitFn g_origHasOutfit = nullptr;
HasDlcForOutfitFn g_origHasDlcForOutfit = nullptr;
HasDlcForOutfitFn g_retailHasDlcForOutfit = nullptr;   // (v0.9.455) the game's own (its trampoline once hooked)
bool Removed(unsigned int index) {
    std::lock_guard<std::mutex> lock(g_appliedMutex);
    return std::find(g_removedIndexes.begin(), g_removedIndexes.end(), index) != g_removedIndexes.end();
}

// (v0.9.455) Every retail outfit is available at the campsite from the start, whatever it needs in the game (an unlock
// trait from the story, a challenge or a collection, crafting, the profile's unlock bits): Crafting::HasOutfit (retail
// 0x453010; TR2 0x4820E0: the outfit's dtp::OutfitDescription +0x24 unlock trait, or for the DLC unlock types 2 / 3
// DLCSystem::CheckIfFeatureIsUsable(+0x28)) and PlayerProfile::IsOutfitUnlocked (0x862FC0: the profile's bits +0x4668)
// answer yes. Only a DLC's own outfits keep asking for their DLC: Crafting::HasDLCForOutfit (0x452F80; TR2 0x482050) is
// true for every other unlock type and DLCSystem's answer for those, so an outfit whose content the game cannot use is
// not offered. A mod's deleted outfits stay deleted.
bool AlwaysAvailable(unsigned int index) {
    if (index == 0xFFFFFFFFu || Removed(index)) return false;
    if (AppliedConfigForIndex(index) >= 0 || ForcedUnlocked(index)) return true;
    const auto hasDlc = g_origHasDlcForOutfit ? g_origHasDlcForOutfit : g_retailHasDlcForOutfit;
    return hasDlc && hasDlc(index);
}

bool __fastcall HookIsOutfitUnlocked(void* profile, unsigned int index) {
    if (AlwaysAvailable(index)) return true;
    return g_origIsOutfitUnlocked(profile, index);
}

bool __fastcall HookHasOutfit(void* instance, unsigned int index) {
    if (index != 0xFFFFFFFFu && Removed(index)) return false;
    if (AlwaysAvailable(index)) return true;
    return g_origHasOutfit(instance, index);
}
using CustomImageLoadFn = void*(__fastcall*)(void*, const char*);
CustomImageLoadFn g_origCustomImageLoad = nullptr;
bool g_imageLoadHooked = false;
std::atomic<ImageLoadedFn> g_imageLoadedListener{nullptr};
std::atomic<int> g_imageLoadsLogged{0};
void* __fastcall HookCustomImageLoad(void* loader, const char* url) {
    void* image = g_origCustomImageLoad(loader, url);
    char text[128]{};
    for (std::size_t k = 0; url && k + 1 < sizeof(text); ++k)
        if (!CopyGuarded(&text[k], url + k, 1) || !text[k]) break;
    std::uint32_t size[2]{};
    if (image) CopyGuarded(size, static_cast<const std::uint8_t*>(image) + 0x30, sizeof(size));
    // One of our larger pictures: shown at the game's size (the menu draws an image at its own size)
    bool scaled = false;
    if (image && std::strncmp(text, "img://", 6) == 0 && size[0] > 128) {
        const char* digits = text + 6;
        if (*digits == '#') ++digits;
        const auto id = static_cast<std::uint32_t>(std::strtoul(digits, nullptr, 10));
        std::lock_guard<std::mutex> lock(g_largePictureMutex);
        for (const auto& [texture, dims] : g_largePictures)
            if (texture == id && dims.first == size[0] && dims.second == size[1]) {
                const std::uint32_t shown[2]{128, static_cast<std::uint32_t>(std::lround(double(size[1]) * 128.0 / double(size[0])))};
                // the GImageInfo (+0x30 width, +0x34 height) and its ScaleformTexture (+0x18; +0x50 width, +0x54
                // height): both say the game's size, so the texture coordinates still span the whole texture
                std::memcpy(static_cast<std::uint8_t*>(image) + 0x30, shown, sizeof(shown));
                std::uint8_t* texture = nullptr;
                if (CopyGuarded(&texture, static_cast<const std::uint8_t*>(image) + 0x18, sizeof(texture)) && texture) {
                    std::uint32_t was[2]{};
                    if (CopyGuarded(was, texture + 0x50, sizeof(was)) && was[0] == size[0] && was[1] == size[1])
                        std::memcpy(texture + 0x50, shown, sizeof(shown));
                }
                scaled = true;
            }
    }
    // (a line for what is worth knowing: an image not found, or one shown smaller than its texture)
    if (!image && g_imageLoadsLogged.fetch_add(1) < 80)
        Log::Writef("CustomOutfits: image %s: not found (no texture of that ID loaded)", text);
    else if (scaled)
        Log::Detailf("CustomOutfits: image %s: %ux%u, shown at the game's 128x%u", text, size[0], size[1],
                    unsigned(std::lround(double(size[1]) * 128.0 / double(size[0]))));
    if (const ImageLoadedFn listener = g_imageLoadedListener.load()) listener(url, image);   // (the other module's look)
    return image;
}
using IsOutfitCraftableFn = bool(__fastcall*)(void*, const std::uint8_t*);
IsOutfitCraftableFn g_origIsOutfitCraftable = nullptr;
bool __fastcall HookIsOutfitCraftable(void* menu, const std::uint8_t* outfit) {
    if (outfit && !g_removed.empty()) {
        const char* debugName = nullptr;
        char name[64]{};
        if (CopyGuarded(&debugName, outfit + kOutfitDebugName, sizeof(debugName)))
            for (std::size_t k = 0; debugName && k + 1 < sizeof(name); ++k)
                if (!CopyGuarded(&name[k], debugName + k, 1) || !name[k]) break;
        for (const auto& r : g_removed)
            if (name[0] && _stricmp(r.name.c_str(), name) == 0) return false;
    }
    return g_origIsOutfitCraftable(menu, outfit);
}
bool __fastcall HookHasDlcForOutfit(unsigned int index) {
    if (AppliedConfigForIndex(index) >= 0 || ForcedUnlocked(index)) return true;
    return g_origHasDlcForOutfit(index);
}

// ---- toggles: mesh parts of the worn custom outfit shown or hidden -------------------------------------------------
std::mutex g_toggleMutex;                 // OutfitConfig::Toggle::on (the UI writes, the game thread reads)
std::atomic<int> g_wornConfig{-1};        // the custom outfit with toggles the player wears (-1: none)
std::vector<Features::HiddenModelPart> g_toggleHidden;   // the parts the toggles hid (game thread; sorted)
bool g_toggleTrackerMissing = false;      // (said once: parts cannot be hidden without the model instance tracker)
std::atomic<int> g_menuConfig{-1};        // the outfit whose parts the Outfits screen changed (first choice while open)

void SaveToggle(const OutfitConfig& config, const OutfitConfig::Toggle& toggle) {
    std::error_code error;
    fs::create_directories(g_statePath.parent_path(), error);
    const std::string key = config.key + "/" + toggle.name;
    WritePrivateProfileStringA("Toggles", key.c_str(), toggle.on ? "1" : "0", g_statePath.string().c_str());
}

// (v0.9.454) Toggle `index` of `config` shown or hidden, and saved. Shown, the shown toggles it is never shown with are
// hidden and saved too (their indices in `hidden`). False: it already was.
bool ChangeToggle(OutfitConfig& config, std::size_t index, bool on, std::vector<std::size_t>* hidden = nullptr) {
    auto& toggle = config.toggles[index];
    std::vector<std::size_t> others;
    {
        std::lock_guard<std::mutex> lock(g_toggleMutex);
        if (toggle.on == on) return false;
        toggle.on = on;
        if (on)
            for (std::size_t k = 0; k < config.toggles.size(); ++k)
                if (k != index && config.toggles[k].on && NeverShownTogether(toggle, config.toggles[k])) {
                    config.toggles[k].on = false;
                    others.push_back(k);
                }
    }
    SaveToggle(config, toggle);
    for (const std::size_t k : others) {
        SaveToggle(config, config.toggles[k]);
        Log::Detailf("CustomOutfits: %s: showing %s hides %s (never shown together)", config.key.c_str(), toggle.name.c_str(),
                     config.toggles[k].name.c_str());
    }
    if (hidden) *hidden = std::move(others);
    return true;
}

void UpdateToggles() {
    bool any = false;
    for (const auto& config : g_configs) any = any || !config.toggles.empty();
    if (!any && g_toggleHidden.empty()) return;
    if (!Features::MaterialResourceVisibilityAvailable() || Features::OutfitSwapInProgress()) return;
    const std::uint16_t worn = Features::CurrentOutfitObjectId();
    if (!worn) return;   // no player right now: keep what is applied

    // Several custom outfits can wear one object: the one picked last wins.
    int wornConfig = -1;
    const int menu = g_menuConfig.load();
    if (menu >= 0 && static_cast<std::size_t>(menu) < g_configs.size() && g_configs[menu].object == worn &&
        !g_configs[menu].toggles.empty())
        wornConfig = menu;
    const int last = g_lastWornConfig.load();
    if (wornConfig < 0 && last >= 0 && static_cast<std::size_t>(last) < g_configs.size() && g_configs[last].object == worn &&
        !g_configs[last].toggles.empty())
        wornConfig = last;
    for (std::size_t i = 0; wornConfig < 0 && i < g_configs.size(); ++i)
        if (g_configs[i].object == worn && !g_configs[i].toggles.empty()) wornConfig = static_cast<int>(i);
    g_wornConfig.store(wornConfig);

    std::vector<Features::HiddenModelPart> wanted;
    bool watch = false;   // (v0.9.602) a toggle of the worn outfit follows a holstered weapon
    if (wornConfig >= 0) {
        // (v0.9.602) the weapon types Lara has holstered, read on this thread's tick (features.cpp, before this one)
        const int holstered = Features::HolsteredWeaponClasses();
        std::lock_guard<std::mutex> lock(g_toggleMutex);
        for (const auto& toggle : g_configs[static_cast<std::size_t>(wornConfig)].toggles) {
            watch = watch || toggle.holstered != 0;
            // (v0.9.604) any of its weapon types holstered: shown, or hidden ("holster_hides")
            const bool any = (unsigned(holstered) & toggle.holstered) != 0;
            if (!toggle.on || (toggle.holstered && any == toggle.holsterHides))
                wanted.insert(wanted.end(), toggle.parts.begin(), toggle.parts.end());
        }
    }
    Features::SetHolsterWatch(watch);
    std::sort(wanted.begin(), wanted.end());
    wanted.erase(std::unique(wanted.begin(), wanted.end()), wanted.end());
    if (wanted == g_toggleHidden) return;
    if (!Features::SetHiddenModelParts(wanted)) {   // (retried next tick)
        if (!g_toggleTrackerMissing)
            Log::Write("CustomOutfits: toggles cannot hide parts: the model instance tracker is not installed");
        g_toggleTrackerMissing = true;
        return;
    }
    g_toggleHidden = std::move(wanted);
}

// v0.9.451: Lara's gear not drawn while the outfit worn says so (mod.json "gear": false): of the outfits wearing her
// object, the one changed on the Outfits screen, else the one picked last, else the first (as her toggles go).
// v0.9.455: by markers: the lower gear's (kLowerGearMarkers, "hide_lower_gear"), the upper gear's (kUpperGearMarkers,
// "hide_upper_gear": the weapons on her back) and any of hers ("hide_markers"). laracroft's marker IDs (DTP 267228's
// markers; every outfit's object has them: the items find their markers by them).
constexpr std::int32_t kLowerGearMarkers[] = {
    24,    // marker_PDA: g_walkie_talkie
    18,    // marker_pistolOneAway: wp_pistol and its variants
    19,    // marker_pistolTwoAway: the offhand pistol (dual wield)
    17,    // marker_pickaxeAway: wp_climbing_axe and its variants, wp_pry_base (and her hip's wp_axe_phys with it)
    29,    // marker_GlowstickAway-brandoz1: g_glowstick, g_flare, g_rebreather (meshes only: its light kept)
    405,   // collisionmarker_WaistAttach_knife-benriz1: g_knife
    404,   // ..._knife_sheath-benriz1: g_knife_sheath
    299,   // ..._rope-benriz1: g_rope
    295,   // ..._CartridgePouch-benriz1: g_cartridgepouch_belt(_large)
    296,   // ..._CartridgePouch-benriz2: g_magazinebeltpouch_large / _small
    450,   // marker_UziAway-carmst1: wp_pistol_microuzi, the Hailstorm (v0.9.463)
};
constexpr std::int32_t kQuiverMarker = 294;   // (v0.9.589) g_quiver_*: her quiver
constexpr std::int32_t kUpperGearMarkers[] = {
    21,    // marker_bowAway: the bows (and g_ascender)
    176,   // marker_bowAway_ledge: the bow put away while she hangs from a ledge
    168,   // marker_SMGAway: the rifles
    430,   // marker_SMGAway-agarttme1
    444,   // marker_ShotgunFullAuto_Away-agarttme1
    445,   // marker_ShotgunTactical_Away-agarttme1
    294,   // collisionmarker_BackAttachQuiver-benriz1: g_quiver_* (v0.9.591)
};

void UpdateLaraGear() {
    const std::uint16_t worn = Features::CurrentOutfitObjectId();
    if (!worn || !Features::PlayerGearHidingAvailable()) return;   // (no player right now: keep what is applied)
    int config = -1;
    const int menu = g_menuConfig.load();
    if (menu >= 0 && static_cast<std::size_t>(menu) < g_configs.size() && g_configs[menu].object == worn) config = menu;
    const int last = g_lastWornConfig.load();
    if (config < 0 && last >= 0 && static_cast<std::size_t>(last) < g_configs.size() && g_configs[last].object == worn)
        config = last;
    for (std::size_t i = 0; config < 0 && i < g_configs.size(); ++i)
        if (g_configs[i].object == worn) config = static_cast<int>(i);
    std::vector<std::int32_t> markers;
    if (config >= 0) {
        const OutfitConfig& c = g_configs[static_cast<std::size_t>(config)];
        if (c.hideLowerGear) markers.insert(markers.end(), std::begin(kLowerGearMarkers), std::end(kLowerGearMarkers));
        if (c.hideUpperGear) markers.insert(markers.end(), std::begin(kUpperGearMarkers), std::end(kUpperGearMarkers));
        markers.insert(markers.end(), c.hideMarkers.begin(), c.hideMarkers.end());
    }
    const bool outfitHides = !markers.empty();
    // (v0.9.586) Enhancements > Gameplay > Hide Lower Gear / Hide Upper Gear: the same markers, whatever the outfit
    const bool lower = Settings::Get().hideLowerGear, upper = Settings::Get().hideUpperGear;
    if (lower) markers.insert(markers.end(), std::begin(kLowerGearMarkers), std::end(kLowerGearMarkers));
    if (upper) markers.insert(markers.end(), std::begin(kUpperGearMarkers), std::end(kUpperGearMarkers));
    // (v0.9.589) her quiver with "Don't draw the weapon switched to" while the chosen weapon is not a bow
    if (Features::QuiverHiddenForChosenWeapon()) markers.push_back(kQuiverMarker);
    std::sort(markers.begin(), markers.end());
    markers.erase(std::unique(markers.begin(), markers.end()), markers.end());
    static int s_global = 0;   // (bit 0 lower, bit 1 upper: what was said last)
    const int global = (lower ? 1 : 0) | (upper ? 2 : 0);
    if (global != s_global) {
        s_global = global;
        Log::Writef("CustomOutfits: Enhancements > Gameplay: %s", global == 0 ? "Lara's gear drawn as her outfit says"
                                                                 : global == 1 ? "her lower gear not drawn"
                                                                 : global == 2 ? "her upper gear not drawn"
                                                                               : "her lower and upper gear not drawn");
    }
    static int s_hiding = -1;   // the outfit Lara's gear is hidden for (-1: none)
    if ((outfitHides ? config : -1) != s_hiding) {
        if (outfitHides) {
            const OutfitConfig& c = g_configs[static_cast<std::size_t>(config)];
            Log::Writef("CustomOutfits: %s: not drawn while it is worn: %s%s%s%s", c.key.c_str(),
                        c.hideLowerGear ? "Lara's lower gear" : "", c.hideLowerGear && c.hideUpperGear ? ", " : "",
                        c.hideUpperGear ? "her upper gear" : "",
                        c.hideMarkers.empty() ? ""
                                              : ((c.hideLowerGear || c.hideUpperGear ? ", " : "") +
                                                 std::to_string(c.hideMarkers.size()) + " marker(s) of hers").c_str());
        } else {
            Log::Detail("CustomOutfits: Lara's gear drawn again");
        }
        s_hiding = outfitHides ? config : -1;
    }
    Features::SetPlayerGearHiddenGameThread(markers);
}

bool CreateHook(std::uintptr_t rva, const std::uint8_t* sig, std::size_t sigSize, void* detour, void** original,
                const char* name) {
    if (!GameBuild::VerifyBytes(rva, sig, sigSize)) {
        Log::Writef("CustomOutfits: %s signature mismatch", name);
        return false;
    }
    if (MH_CreateHook(reinterpret_cast<void*>(GameBuild::Base() + rva), detour, original) != MH_OK) {
        Log::Writef("CustomOutfits: %s hook could not be created", name);
        return false;
    }
    return true;
}
// ---- "Custom parts": the worn outfit's toggles as a third tile of the campsite's Inventory --------------------------
// The Inventory (UIMenuStateCampsiteCraftingSelector) is the Skills selector's movie with two of its three tiles
// filled; its grid (UiMenuStateCampsiteCrafting) lists the crafting recipes of one dtp::CraftingType (4 Ammunition, 5
// Equipment). The worn outfit's toggles are appended to the game's recipe list as recipes of type 7 (no requirements,
// never crafted) and shown on the third tile; selecting one shows or hides the part instead of crafting it. Read from
// TR2_x64_final.exe with its PDB; the retail functions found by their bytes (docs/materials.md).
constexpr std::uintptr_t kRvaSelectorOnEnter = 0x009024E0ull;      // UIMenuStateCampsiteCraftingSelector::OnEnter
constexpr std::uintptr_t kRvaSelectorOnUiSelect = 0x0092DB60ull;   // UIMenuStateCampsiteCraftingSelector::OnUiSelect
constexpr std::uintptr_t kRvaCraftingOnEnter = 0x00906C50ull;      // UiMenuStateCampsiteCrafting::OnEnter
constexpr std::uintptr_t kRvaCraftingOnExit = 0x00912310ull;       // UiMenuStateCampsiteCrafting::OnExit
constexpr std::uintptr_t kRvaPopulateSkills = 0x00938C40ull;       // UiMenuStateCampsiteCrafting::PopulateSkills
constexpr std::uintptr_t kRvaCraftingOnUiSelect = 0x0092F720ull;   // UiMenuStateCampsiteCrafting::OnUiSelect
constexpr std::uintptr_t kRvaUpdateInformation = 0x0095A620ull;    // ::UpdateInformationWidgetAndButtonLegend
constexpr std::uintptr_t kRvaSetupNavigationMenu = 0x0094C2B0ull;  // UiMenuStateCampsiteCrafting::SetupNavigationMenu
constexpr std::uintptr_t kRvaInvokeStateMovie = 0x008B9750ull;     // UiMenuStateBase::InvokeStateMovie
constexpr std::uintptr_t kRvaRegisterNavItems = 0x008BEC80ull;     // UiMenuStateGeneric::RegisterNavItems
constexpr std::uintptr_t kRvaSetHeaderAndSub = 0x009BF110ull;      // UiWidgetSSNavMenu::SetHeaderAndSub
constexpr std::uintptr_t kRvaSetHeaderIcon = 0x009BF260ull;        // UiWidgetSSNavMenu::SetHeaderIcon
constexpr std::uintptr_t kRvaWidgetManagerGet = 0x008B35E0ull;     // UiWidgetManager::Get
constexpr std::uintptr_t kRvaGetWidget = 0x0007A560ull;            // cdc::UiWidgetManagerBase::GetWidget
constexpr std::uintptr_t kRvaSendCraftingTitle = 0x009B85D0ull;    // UiWidgetGearLoadoutInformation::SendCrafting...
constexpr std::uintptr_t kRvaInvokeASFunction = 0x00995D80ull;     // UiWidgetScaleform::InvokeASFunction
constexpr std::uintptr_t kRvaGfxGetMember = 0x00AF4A70ull;         // GFxValue::ObjectInterface::GetMember
constexpr std::uintptr_t kRvaGfxInvoke = 0x00AF4F00ull;            // GFxValue::ObjectInterface::Invoke
constexpr std::uintptr_t kRvaGfxObjectRelease = 0x00AF5700ull;     // GFxValue::ObjectInterface::ObjectRelease
constexpr std::uintptr_t kRvaGetRecipe = 0x0044B9F0ull;            // Crafting::GetRecipe(unsigned char)
constexpr std::uint8_t kSelectorOnEnterSig[] = {0x48, 0x89, 0x5C, 0x24, 0x10, 0x48, 0x89, 0x4C, 0x24, 0x08, 0x55, 0x56, 0x57, 0x41, 0x54, 0x41};
constexpr std::uint8_t kSelectorOnUiSelectSig[] = {0x40, 0x53, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x8B, 0xD9, 0xC7, 0x44, 0x24, 0x30, 0x5B, 0x00, 0x00};
constexpr std::uint8_t kCraftingOnEnterSig[] = {0x4C, 0x89, 0x44, 0x24, 0x18, 0x48, 0x89, 0x54, 0x24, 0x10, 0x55, 0x53, 0x56, 0x57, 0x41, 0x54};
constexpr std::uint8_t kCraftingOnExitSig[] = {0x48, 0x89, 0x5C, 0x24, 0x10, 0x48, 0x89, 0x6C, 0x24, 0x18, 0x48, 0x89, 0x74, 0x24, 0x20, 0x57};
constexpr std::uint8_t kPopulateSkillsSig[] = {0x48, 0x8B, 0xC4, 0x55, 0x53, 0x56, 0x48, 0x8D, 0x68, 0xA1, 0x48, 0x81, 0xEC, 0xC0, 0x00, 0x00};
constexpr std::uint8_t kCraftingOnUiSelectSig[] = {0x48, 0x8B, 0xC4, 0x55, 0x53, 0x56, 0x41, 0x54, 0x48, 0x8D, 0xA8, 0x08, 0xFC, 0xFF, 0xFF, 0x48};
constexpr std::uint8_t kUpdateInformationSig[] = {0x48, 0x89, 0x4C, 0x24, 0x08, 0x55, 0x53, 0x56, 0x57, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57, 0x48};
constexpr std::uint8_t kSetupNavigationMenuSig[] = {0x48, 0x89, 0x5C, 0x24, 0x10, 0x48, 0x89, 0x74, 0x24, 0x18, 0x57, 0x48, 0x81, 0xEC, 0x30, 0x04};
constexpr std::uint8_t kInvokeStateMovieSig[] = {0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x74, 0x24, 0x10, 0x57, 0x48, 0x81, 0xEC, 0x40, 0x01};
constexpr std::uint8_t kRegisterNavItemsSig[] = {0x4C, 0x8B, 0xDC, 0x49, 0x89, 0x5B, 0x10, 0x49, 0x89, 0x73, 0x18, 0x55, 0x41, 0x56, 0x41, 0x57};
constexpr std::uint8_t kSetHeaderAndSubSig[] = {0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x6C, 0x24, 0x10, 0x48, 0x89, 0x74, 0x24, 0x18, 0x48};
constexpr std::uint8_t kWidgetManagerGetSig[] = {0x48, 0x8B, 0x05, 0xE9, 0x2F, 0x84, 0x00, 0xC3};
constexpr std::uint8_t kGetWidgetSig[] = {0x48, 0x83, 0x79, 0x18, 0x00, 0x4C, 0x63, 0xC2, 0x74, 0x24, 0x33, 0xD2, 0x49, 0x8B, 0xC0, 0x48};
constexpr std::uint8_t kSendCraftingTitleSig[] = {0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x74, 0x24, 0x10, 0x48, 0x89, 0x7C, 0x24, 0x18, 0x4C};
constexpr std::uint8_t kInvokeASFunctionSig[] = {0x48, 0x85, 0xD2, 0x0F, 0x84, 0xC0, 0x00, 0x00, 0x00, 0x48, 0x8B, 0xC4, 0x48, 0x89, 0x58, 0x10};
constexpr std::uint8_t kGfxGetMemberSig[] = {0x48, 0x89, 0x5C, 0x24, 0x18, 0x56, 0x57, 0x41, 0x56, 0x48, 0x83, 0xEC, 0x40, 0x80, 0xBC, 0x24};
constexpr std::uint8_t kGfxInvokeSig[] = {0x40, 0x53, 0x41, 0x54, 0x41, 0x56, 0x41, 0x57, 0x48, 0x81, 0xEC, 0xA8, 0x00, 0x00, 0x00, 0x80};
constexpr std::uint8_t kGfxObjectReleaseSig[] = {0x40, 0x53, 0x48, 0x83, 0xEC, 0x20, 0x8B, 0x42, 0x08, 0x49, 0x8B, 0xD8, 0x25, 0x8F, 0x00, 0x00};
constexpr std::uint8_t kGetRecipeSig[] = {0x0F, 0xB6, 0xC1, 0x48, 0x8B, 0x0D, 0x36, 0xBA, 0x90, 0x02, 0x48, 0x8D, 0x04, 0x40, 0x48, 0xC1};

// Offsets (TR2_x64_final with its PDB; the retail functions read the same ones).
constexpr std::ptrdiff_t kStateSelectedIndex = 0x2C;   // int: the highlighted nav item
constexpr std::ptrdiff_t kStateNavCount = 0x38;        // int: how many nav items (the selector: 2 tiles)
constexpr std::ptrdiff_t kStateMovieValue = 0x60;      // GFxValue: the state's movie clip
constexpr std::ptrdiff_t kStateInfoWidget = 0x150;     // int: the information panel's widget ID
constexpr std::ptrdiff_t kCraftingRecipes = 0x170;     // u16[]: the grid's recipe indexes (read back as bytes)
constexpr std::ptrdiff_t kCraftingCount = 0x1B1;       // u8: how many
constexpr std::ptrdiff_t kCraftingCategory = 0x200;    // dtp::CraftingType
constexpr std::ptrdiff_t kCraftingCanCraft = 0x20D;    // u8
constexpr std::ptrdiff_t kCraftingNumRecipe = 0x00;    // dtp::GlobalCrafting
constexpr std::ptrdiff_t kCraftingRecipeList = 0x08;
constexpr std::size_t kRecipeSize = 0xC0;              // dtp::CraftingRecipe
constexpr int kCustomCategory = 7;                     // (dtp::CraftingType ends at 6, OutfitCrafting)
constexpr unsigned int kMaxRecipes = 256;              // the grid keeps recipe indexes as bytes

struct GfxValue {   // Scaleform GFx 3 GFxValue
    void* iface = nullptr;
    std::uint32_t type = 0;   // 0 undefined, 2 bool, 3 number, 4 string, 8 display object; 0x40 managed
    std::uint32_t pad = 0;
    union {
        double number;
        const char* string;
        void* data;
        bool boolean;
    };
    GfxValue() : data(nullptr) {}
};
static_assert(sizeof(GfxValue) == 24, "GFxValue");

using StateFn = std::uintptr_t(__fastcall*)(void*, void*, void*, void*);
using InvokeStateMovieFn = bool(__fastcall*)(void*, const char*, GfxValue*, const GfxValue*, unsigned int);
using RegisterNavItemsFn = void(__fastcall*)(void*);
using SetHeaderAndSubFn = void(__fastcall*)(void*, const char*, const char*);
using SetHeaderIconFn = void(__fastcall*)(void*, const char*);
using WidgetManagerGetFn = void*(__fastcall*)();
using GetWidgetFn = void*(__fastcall*)(void*, int);
using SendCraftingTitleFn = void(__fastcall*)(void*, const void*);
using InvokeASFunctionFn = void(__fastcall*)(void*, const char*, GfxValue*, const GfxValue*, unsigned int);
using GfxGetMemberFn = bool(__fastcall*)(void*, void*, const char*, GfxValue*, bool);
using GfxInvokeFn = bool(__fastcall*)(void*, void*, GfxValue*, const char*, const GfxValue*, std::size_t, bool);
using GfxObjectReleaseFn = void(__fastcall*)(void*, GfxValue*, void*);
using GetRecipeFn = std::uint8_t*(__fastcall*)(unsigned int);

StateFn g_origSelectorOnEnter = nullptr, g_origSelectorOnUiSelect = nullptr, g_origCraftingOnEnter = nullptr,
        g_origCraftingOnExit = nullptr, g_origPopulateSkills = nullptr, g_origCraftingOnUiSelect = nullptr,
        g_origUpdateInformation = nullptr, g_origSetupNavigationMenu = nullptr;
InvokeStateMovieFn g_origInvokeStateMovie = nullptr;
RegisterNavItemsFn g_origRegisterNavItems = nullptr;
SetHeaderIconFn g_origSetHeaderIcon = nullptr;

bool g_partsEnabled = false;            // the hooks are in ([Outfits] CustomPartsMenu, default on)
bool g_partsShown = false;              // the Inventory opened with the third tile (game thread)
int g_partsConfig = -1;                 // the outfit whose toggles it lists
unsigned int g_partsFirstRecipe = 0;    // the first custom recipe's index
unsigned int g_partsCount = 0;
void* g_selectorEntering = nullptr;     // the selector during its OnEnter
bool g_pendingCustomCategory = false;   // the third tile was chosen: the grid opens on type 7
void* g_craftingState = nullptr;
bool g_inCrafting = false;
std::uint8_t* g_recipeBuffer = nullptr; // our copy of the recipe list (+ the custom recipes)
std::uint8_t* g_gameRecipeList = nullptr;
unsigned int g_gameRecipeCount = 0;
std::mutex g_partTextMutex;
std::vector<std::string> g_partTexts;   // title, description per part (by string ID)
std::vector<std::string> g_partIcons;
std::string g_tileName = "Custom Parts", g_tileSecondLine, g_tileDescription, g_pageHeader = "Custom Parts", g_pageSub;
bool g_partsLoggedOnce = false;

template <typename T>
T Game(std::uintptr_t rva) { return reinterpret_cast<T>(GameBuild::Base() + rva); }
template <typename T>
T& At(void* object, std::ptrdiff_t offset) { return *reinterpret_cast<T*>(static_cast<std::uint8_t*>(object) + offset); }

const char* PartString(int id) {
    std::lock_guard<std::mutex> lock(g_partTextMutex);
    const int index = g_partStringTop - id;
    if (index < 0 || static_cast<std::size_t>(index) >= g_partTexts.size()) return nullptr;
    return g_partTexts[static_cast<std::size_t>(index)].c_str();
}

bool PartShown(unsigned int part) {
    if (g_partsConfig < 0 || static_cast<std::size_t>(g_partsConfig) >= g_configs.size()) return false;
    const auto& config = g_configs[static_cast<std::size_t>(g_partsConfig)];
    std::lock_guard<std::mutex> lock(g_toggleMutex);
    return part < config.menuToggles.size() && config.toggles[config.menuToggles[part]].on;   // (v0.9.604: the menu's)
}

// The texts the game reads (by string ID) and the tile's: rebuilt when the Inventory opens and when a part changes.
void UpdatePartTexts() {
    if (g_partsConfig < 0) return;
    const auto& config = g_configs[static_cast<std::size_t>(g_partsConfig)];
    std::vector<std::string> texts;
    unsigned int shown = 0;
    for (unsigned int i = 0; i < g_partsCount; ++i) {
        const std::size_t index = config.menuToggles[i];   // (v0.9.604: the menu's toggles only)
        const auto& t = config.toggles[index];
        const bool on = PartShown(i);
        shown += on ? 1 : 0;
        texts.push_back(t.title.empty() ? t.name : t.title);
        std::string hides;   // (v0.9.454)
        if (!on) {
            std::lock_guard<std::mutex> lock(g_toggleMutex);
            hides = ShownNotWith(config, index);
        }
        texts.push_back(on ? std::string("Shown. Select to hide it.")
                        : hides.empty() ? std::string("Hidden. Select to show it.")
                                        : "Hidden. Select to show it: that hides " + hides + ".");
    }
    {
        std::lock_guard<std::mutex> lock(g_partTextMutex);
        g_partTexts = std::move(texts);
    }
    char line[64];
    std::snprintf(line, sizeof(line), "%u/%u SHOWN", shown, g_partsCount);
    g_tileSecondLine = line;
    g_pageSub = line;
    g_tileDescription = "Show or hide parts of " + (config.name.empty() ? config.key : config.name) + ".";
}

// The worn outfit's toggles as recipes of type 7 at the end of the game's recipe list (the list is ours: a copy).
bool PrepareCustomParts() {
    g_partsShown = false;
    std::uint8_t* crafting = nullptr;
    if (!CopyGuarded(&crafting, reinterpret_cast<const void*>(GameBuild::Base() + kRvaGlobalCraftingPtr), sizeof(crafting)) ||
        !crafting)
        return false;
    auto& listField = At<std::uint8_t*>(crafting, kCraftingRecipeList);
    auto& countField = At<std::uint32_t>(crafting, kCraftingNumRecipe);
    // the worn outfit: as the toggles find it (UpdateToggles), looked up again here (its tick may not have run)
    int worn = -1;
    const std::uint16_t wornObject = Features::CurrentOutfitObjectId();
    if (wornObject) {
        const int last = g_lastWornConfig.load();
        if (last >= 0 && static_cast<std::size_t>(last) < g_configs.size() && g_configs[last].object == wornObject &&
            !g_configs[last].menuToggles.empty())
            worn = last;
        for (std::size_t i = 0; worn < 0 && i < g_configs.size(); ++i)
            if (g_configs[i].object == wornObject && !g_configs[i].menuToggles.empty()) worn = static_cast<int>(i);
    }
    if (worn < 0) worn = g_wornConfig.load();
    if (worn < 0 || static_cast<std::size_t>(worn) >= g_configs.size() || g_configs[static_cast<std::size_t>(worn)].menuToggles.empty()) {
        // nothing to list: the game's recipes only (the list is left alone until an outfit with toggles is worn)
        if (g_recipeBuffer && listField == g_recipeBuffer) countField = g_gameRecipeCount;
        g_partsConfig = -1;
        g_partsCount = 0;
        std::string configs;
        for (const auto& config : g_configs)
            configs += " " + config.key + "=" + std::to_string(config.object) + "/" + std::to_string(config.toggles.size());
        Log::Detailf("CustomOutfits: custom parts: Inventory opened, no outfit with toggles worn (worn object %u, toggles' "
                    "outfit %d, part hiding %s; outfits (object/toggles):%s)",
                    wornObject, g_wornConfig.load(), Features::MaterialResourceVisibilityAvailable() ? "ready" : "unavailable",
                    configs.c_str());
        return false;
    }
    if (listField != g_recipeBuffer || !g_recipeBuffer) {   // the game's list (first time, or reloaded)
        g_gameRecipeList = listField;
        g_gameRecipeCount = countField;
        if (!g_gameRecipeList || g_gameRecipeCount == 0 || g_gameRecipeCount >= kMaxRecipes) {
            Log::Writef("CustomOutfits: custom parts: the game's recipe list is unusable (%u)", g_gameRecipeCount);
            return false;
        }
        auto buffer = std::make_unique<std::uint8_t[]>(kMaxRecipes * kRecipeSize);
        std::memset(buffer.get(), 0, kMaxRecipes * kRecipeSize);
        std::memcpy(buffer.get(), g_gameRecipeList, g_gameRecipeCount * kRecipeSize);
        g_recipeBuffer = buffer.get();
        g_tableBuffers.push_back(std::move(buffer));
        Log::Detailf("CustomOutfits: custom parts: the game has %u crafting recipes", g_gameRecipeCount);
    }
    g_partsConfig = worn;
    if (g_wornConfig.load() != worn) {
        Log::Detailf("CustomOutfits: custom parts: the toggles' outfit was %d, now %d", g_wornConfig.load(), worn);
        g_wornConfig.store(worn);
    }
    g_partsCount = static_cast<unsigned int>(std::min<std::size_t>(g_configs[static_cast<std::size_t>(worn)].menuToggles.size(), kMaxParts));
    g_partsCount = std::min(g_partsCount, kMaxRecipes - g_gameRecipeCount);
    g_partsFirstRecipe = g_gameRecipeCount;
    // a template: the first Equipment recipe (its sounds and durations)
    const std::uint8_t* model = nullptr;
    for (unsigned int i = 0; i < g_gameRecipeCount && !model; ++i)
        if (At<std::int32_t>(g_recipeBuffer + i * kRecipeSize, 0x60) == 5) model = g_recipeBuffer + i * kRecipeSize;
    if (!model) model = g_recipeBuffer;
    const OutfitConfig* config = g_partsConfig >= 0 ? &g_configs[static_cast<std::size_t>(g_partsConfig)] : nullptr;
    g_partIcons.resize(g_partsCount);
    for (unsigned int i = 0; i < g_partsCount; ++i) {
        std::uint8_t* r = g_recipeBuffer + (g_gameRecipeCount + i) * kRecipeSize;
        std::memcpy(r, model, kRecipeSize);
        At<std::int16_t>(r, 0x00) = static_cast<std::int16_t>(g_partStringTop - static_cast<int>(i) * 2);       // name
        At<std::int16_t>(r, 0x02) = static_cast<std::int16_t>(g_partStringTop - static_cast<int>(i) * 2 - 1);   // description
        At<std::int16_t>(r, 0x04) = 0;                                     // extraString
        g_partIcons[i] = "Crafting_Rucksack";
        At<const char*>(r, 0x10) = g_partIcons[i].c_str();                 // iconType
        At<const char*>(r, 0x18) = g_partIcons[i].c_str();                 // iconTypeCraft
        std::memset(r + 0x20, 0, 24);                                      // xpRewardELII
        At<std::int32_t>(r, 0x38) = static_cast<std::int32_t>((i / 8) * 9 + i % 8);   // sortOrderIndex (8 a row)
        At<std::uint32_t>(r, 0x40) = 0;                                    // numResourceRequirements
        At<void*>(r, 0x48) = nullptr;
        At<std::uint16_t>(r, 0x50) = 0xFFFF;                               // recipeKnownTraitRequirement
        At<std::uint16_t>(r, 0x52) = 0xFFFF;                               // toolTraitRequirement
        At<std::uint32_t>(r, 0x54) = 0;                                    // numOtherTraitRequirements
        At<void*>(r, 0x58) = nullptr;
        At<std::int32_t>(r, 0x60) = kCustomCategory;                       // type
        std::memset(r + 0x68, 0, 32);                                      // typeData
        At<std::uint32_t>(r, 0x8C) = 0;                                    // numCraftingDurations
        At<void*>(r, 0x90) = nullptr;
        At<std::uint16_t>(r, 0xA8) = 0xFFFF;                               // craftedTrait
        At<const char*>(r, 0xB8) = "custom part";                          // debugName
    }
    listField = g_recipeBuffer;
    countField = g_gameRecipeCount + g_partsCount;
    UpdatePartTexts();
    g_partsShown = g_partsCount > 0;
    if (g_partsShown || !g_partsLoggedOnce)
        Log::Detailf("CustomOutfits: custom parts: Inventory opened, %u part(s) of %s (recipes %u..)", g_partsCount,
                    config ? config->key.c_str() : "(no outfit with toggles worn)", g_partsFirstRecipe);
    g_partsLoggedOnce = true;
    return g_partsShown;
}

bool CustomPage(void* state) {
    return g_partsShown && state && state == g_craftingState && g_inCrafting &&
           At<std::int32_t>(state, kCraftingCategory) == kCustomCategory;
}

// A child of a GFx object (released by the caller with Release).
bool GfxChild(const GfxValue& parent, const char* name, GfxValue& out) {
    if (!parent.iface) return false;
    const bool display = (parent.type & 0x8F) == 8;
    return Game<GfxGetMemberFn>(kRvaGfxGetMember)(parent.iface, parent.data, name, &out, display) && out.iface;
}
void GfxRelease(GfxValue& value) {
    if ((value.type & 0x40) && value.iface) Game<GfxObjectReleaseFn>(kRvaGfxObjectRelease)(value.iface, &value, value.data);
    value.iface = nullptr;
    value.type = 0;
}

// The selector's tile icons (its SetCraftingIcon left out: it would shrink the background to two tiles): Ammunition
// (frame 5), Equipment (4), Custom parts (4, the chest).
void SetSelectorIcons(void* selector) {
    GfxValue state;
    state.iface = At<void*>(selector, kStateMovieValue);
    state.type = At<std::uint32_t>(selector, kStateMovieValue + 8);
    state.data = At<void*>(selector, kStateMovieValue + 0x10);
    if (!state.iface || (state.type & 0x8F) <= 1) {
        Log::Write("CustomOutfits: custom parts: the Inventory's movie clip is not an object; tile icons left as they are");
        return;
    }
    const int frames[3] = {5, 4, 4};
    int done = 0;
    for (int i = 0; i < 3; ++i) {
        char name[32];
        std::snprintf(name, sizeof(name), "nav_list_item_%d_mc", i);
        GfxValue item, icons;
        if (GfxChild(state, name, item) && GfxChild(item, "icons_mc", icons)) {
            GfxValue frame;
            frame.type = 3;
            frame.number = frames[i];
            const bool display = (icons.type & 0x8F) == 8;
            if (Game<GfxInvokeFn>(kRvaGfxInvoke)(icons.iface, icons.data, nullptr, "gotoAndStop", &frame, 1, display)) ++done;
        }
        GfxRelease(icons);
        GfxRelease(item);
    }
    if (done != 3) Log::Detailf("CustomOutfits: custom parts: %d of 3 tile icons set", done);
}

bool __fastcall HookInvokeStateMovie(void* state, const char* name, GfxValue* result, const GfxValue* args, unsigned int count) {
    if (name && state && state == g_selectorEntering && g_partsShown) {
        if (std::strcmp(name, "SetCraftingIcon") == 0) return true;   // (the two-tile layout: see SetSelectorIcons)
        const std::string* third = std::strcmp(name, "SetNavElementsNames") == 0        ? &g_tileName
                                   : std::strcmp(name, "SetNavElementsSecondLine") == 0 ? &g_tileSecondLine
                                   : std::strcmp(name, "SetDescriptions") == 0          ? &g_tileDescription
                                                                                        : nullptr;
        if (third && args && count == 3) {
            GfxValue copy[3];
            std::memcpy(copy, args, sizeof(copy));
            copy[2].iface = nullptr;
            copy[2].type = 4;
            copy[2].string = third->c_str();
            return g_origInvokeStateMovie(state, name, result, copy, count);
        }
    }
    if (name && CustomPage(state) && std::strcmp(name, "SwitchToEquipment") == 0 && args && count == 1) {
        GfxValue copy = args[0];   // the Equipment grid's columns
        copy.type = 2;
        copy.boolean = true;
        return g_origInvokeStateMovie(state, name, result, &copy, count);
    }
    return g_origInvokeStateMovie(state, name, result, args, count);
}

void __fastcall HookRegisterNavItems(void* state) {
    if (state && state == g_selectorEntering && g_partsShown && At<std::int32_t>(state, kStateNavCount) == 2)
        At<std::int32_t>(state, kStateNavCount) = 3;
    g_origRegisterNavItems(state);
}

std::uintptr_t __fastcall HookSelectorOnEnter(void* state, void* a, void* b, void* c) {
    PrepareCustomParts();
    g_selectorEntering = state;
    const std::uintptr_t result = g_origSelectorOnEnter(state, a, b, c);
    g_selectorEntering = nullptr;
    if (g_partsShown) SetSelectorIcons(state);
    return result;
}

std::uintptr_t __fastcall HookSelectorOnUiSelect(void* state, void* a, void* b, void* c) {
    g_pendingCustomCategory = g_partsShown && state && At<std::int32_t>(state, kStateSelectedIndex) == 2;
    if (g_pendingCustomCategory) Log::Detail("CustomOutfits: custom parts: tile chosen");
    return g_origSelectorOnUiSelect(state, a, b, c);
}

std::uintptr_t __fastcall HookCraftingOnEnter(void* state, void* a, void* b, void* c) {
    g_craftingState = state;
    g_inCrafting = true;
    if (g_pendingCustomCategory && state) {
        At<std::int32_t>(state, kCraftingCategory) = kCustomCategory;
        g_pendingCustomCategory = false;
    }
    return g_origCraftingOnEnter(state, a, b, c);
}

std::uintptr_t __fastcall HookCraftingOnExit(void* state, void* a, void* b, void* c) {
    const std::uintptr_t result = g_origCraftingOnExit(state, a, b, c);
    if (state == g_craftingState) g_inCrafting = false;
    return result;
}

// The custom part a grid item is (-1: a game recipe).
int PartOfItem(void* state, int item) {
    if (item < 0 || item >= At<std::uint8_t>(state, kCraftingCount)) return -1;
    const unsigned int recipe = At<std::uint16_t>(state, kCraftingRecipes + item * 2) & 0xFF;
    if (recipe < g_partsFirstRecipe || recipe >= g_partsFirstRecipe + g_partsCount) return -1;
    return static_cast<int>(recipe - g_partsFirstRecipe);
}

void SetItemState(void* state, int item, bool shown) {
    GfxValue args[2];
    args[0].type = 3;
    args[0].number = item;
    args[1].type = 4;
    args[1].string = shown ? "Complete" : "HighlightOff";
    g_origInvokeStateMovie(state, "SetNavItemToState", nullptr, args, 2);
}

std::uintptr_t __fastcall HookPopulateSkills(void* state, void* a, void* b, void* c) {
    const std::uintptr_t result = g_origPopulateSkills(state, a, b, c);
    if (CustomPage(state)) {
        const int count = At<std::uint8_t>(state, kCraftingCount);
        for (int i = 0; i < count; ++i)
            if (const int part = PartOfItem(state, i); part >= 0 && PartShown(static_cast<unsigned int>(part)))
                SetItemState(state, i, true);
        Log::Detailf("CustomOutfits: custom parts: grid of %d item(s)", count);
    }
    return result;
}

void ShowPartInformation(void* state) {
    At<std::uint8_t>(state, kCraftingCanCraft) = 0;
    void* manager = Game<WidgetManagerGetFn>(kRvaWidgetManagerGet)();
    void* widget = manager ? Game<GetWidgetFn>(kRvaGetWidget)(manager, At<std::int32_t>(state, kStateInfoWidget)) : nullptr;
    const int item = At<std::int32_t>(state, kStateSelectedIndex);
    if (!widget || PartOfItem(state, item) < 0) return;
    const unsigned int recipe = At<std::uint16_t>(state, kCraftingRecipes + item * 2) & 0xFF;
    Game<SendCraftingTitleFn>(kRvaSendCraftingTitle)(widget, Game<GetRecipeFn>(kRvaGetRecipe)(recipe));
    Game<InvokeASFunctionFn>(kRvaInvokeASFunction)(widget, "Reset", nullptr, nullptr, 0);
}

std::uintptr_t __fastcall HookUpdateInformation(void* state, void* a, void* b, void* c) {
    if (CustomPage(state)) {
        ShowPartInformation(state);
        return 0;
    }
    return g_origUpdateInformation(state, a, b, c);
}

std::uintptr_t __fastcall HookCraftingOnUiSelect(void* state, void* a, void* b, void* c) {
    if (!CustomPage(state)) return g_origCraftingOnUiSelect(state, a, b, c);
    const int item = At<std::int32_t>(state, kStateSelectedIndex);
    const int part = PartOfItem(state, item);
    if (part < 0 || g_partsConfig != g_wornConfig.load()) return 0;
    const bool shown = !PartShown(static_cast<unsigned int>(part));
    std::vector<int> alsoHidden;   // (v0.9.454: the parts it is never shown with)
    if (!SetWornToggle(part, shown, &alsoHidden)) return 0;
    const auto& partsConfig = g_configs[static_cast<std::size_t>(g_partsConfig)];
    Log::Detailf("CustomOutfits: custom parts: %s %s", partsConfig.toggles[partsConfig.menuToggles[std::size_t(part)]].name.c_str(),
                shown ? "shown" : "hidden");
    UpdatePartTexts();
    SetItemState(state, item, shown);
    if (!alsoHidden.empty()) {
        const int count = At<std::uint8_t>(state, kCraftingCount);
        for (int i = 0; i < count; ++i)
            if (const int other = PartOfItem(state, i);
                other >= 0 && std::find(alsoHidden.begin(), alsoHidden.end(), other) != alsoHidden.end())
                SetItemState(state, i, false);
    }
    ShowPartInformation(state);
    if (void* nav = reinterpret_cast<void*(__fastcall*)(void*)>((*reinterpret_cast<void***>(state))[0x100 / 8])(state))
        Game<SetHeaderAndSubFn>(kRvaSetHeaderAndSub)(nav, g_pageHeader.c_str(), g_pageSub.c_str());
    return 0;
}

std::uintptr_t __fastcall HookSetupNavigationMenu(void* state, void* a, void* b, void* c) {
    const std::uintptr_t result = g_origSetupNavigationMenu(state, a, b, c);
    if (CustomPage(state))
        if (void* nav = reinterpret_cast<void*(__fastcall*)(void*)>((*reinterpret_cast<void***>(state))[0x100 / 8])(state))
            Game<SetHeaderAndSubFn>(kRvaSetHeaderAndSub)(nav, g_pageHeader.c_str(), g_pageSub.c_str());
    return result;
}

void __fastcall HookSetHeaderIcon(void* nav, const char* icon) {
    if (icon && std::strcmp(icon, "Arrow") == 0 && CustomPage(g_craftingState)) icon = "Equipment";
    g_origSetHeaderIcon(nav, icon);
}

void InstallCustomParts() {
    char exe[MAX_PATH]{};
    GetModuleFileNameA(nullptr, exe, MAX_PATH);
    const std::string ini = (fs::path(exe).parent_path() / "ROTTRFramework.ini").string();
    if (!GetPrivateProfileIntA("Outfits", "CustomPartsMenu", 1, ini.c_str())) {
        Log::Detail("CustomOutfits: custom parts: off ([Outfits] CustomPartsMenu=0)");
        return;
    }
    struct Called {
        std::uintptr_t rva;
        const std::uint8_t* sig;
        std::size_t size;
        const char* name;
    };
    const Called called[] = {
        {kRvaSetHeaderAndSub, kSetHeaderAndSubSig, sizeof(kSetHeaderAndSubSig), "UiWidgetSSNavMenu::SetHeaderAndSub"},
        {kRvaWidgetManagerGet, kWidgetManagerGetSig, sizeof(kWidgetManagerGetSig), "UiWidgetManager::Get"},
        {kRvaGetWidget, kGetWidgetSig, sizeof(kGetWidgetSig), "UiWidgetManagerBase::GetWidget"},
        {kRvaSendCraftingTitle, kSendCraftingTitleSig, sizeof(kSendCraftingTitleSig), "SendCraftingTitleDescAndIcon"},
        {kRvaInvokeASFunction, kInvokeASFunctionSig, sizeof(kInvokeASFunctionSig), "UiWidgetScaleform::InvokeASFunction"},
        {kRvaGfxGetMember, kGfxGetMemberSig, sizeof(kGfxGetMemberSig), "GFxValue::ObjectInterface::GetMember"},
        {kRvaGfxInvoke, kGfxInvokeSig, sizeof(kGfxInvokeSig), "GFxValue::ObjectInterface::Invoke"},
        {kRvaGfxObjectRelease, kGfxObjectReleaseSig, sizeof(kGfxObjectReleaseSig), "GFxValue::ObjectInterface::ObjectRelease"},
        {kRvaGetRecipe, kGetRecipeSig, sizeof(kGetRecipeSig), "Crafting::GetRecipe"},
    };
    for (const auto& c : called)
        if (!GameBuild::VerifyBytes(c.rva, c.sig, c.size)) {
            Log::Writef("CustomOutfits: custom parts: %s signature mismatch; the Inventory is left as it is", c.name);
            return;
        }
    // SetHeaderIcon: the retail function (its first bytes are shared with others: checked by its call in
    // SetupNavigationMenu instead).
    const std::uint8_t* call = reinterpret_cast<const std::uint8_t*>(GameBuild::Base() + kRvaSetupNavigationMenu + 0xB7);
    std::int32_t rel = 0;
    std::memcpy(&rel, call + 1, 4);
    if (call[0] != 0xE8 || kRvaSetupNavigationMenu + 0xB7 + 5 + rel != kRvaSetHeaderIcon) {
        Log::Write("CustomOutfits: custom parts: UiWidgetSSNavMenu::SetHeaderIcon not found; the Inventory is left as it is");
        return;
    }
    struct Hook {
        std::uintptr_t rva;
        const std::uint8_t* sig;
        std::size_t size;
        void* detour;
        void** original;
        const char* name;
    };
    const Hook hooks[] = {
        {kRvaSelectorOnEnter, kSelectorOnEnterSig, sizeof(kSelectorOnEnterSig), reinterpret_cast<void*>(&HookSelectorOnEnter),
         reinterpret_cast<void**>(&g_origSelectorOnEnter), "CraftingSelector::OnEnter"},
        {kRvaSelectorOnUiSelect, kSelectorOnUiSelectSig, sizeof(kSelectorOnUiSelectSig), reinterpret_cast<void*>(&HookSelectorOnUiSelect),
         reinterpret_cast<void**>(&g_origSelectorOnUiSelect), "CraftingSelector::OnUiSelect"},
        {kRvaCraftingOnEnter, kCraftingOnEnterSig, sizeof(kCraftingOnEnterSig), reinterpret_cast<void*>(&HookCraftingOnEnter),
         reinterpret_cast<void**>(&g_origCraftingOnEnter), "CampsiteCrafting::OnEnter"},
        {kRvaCraftingOnExit, kCraftingOnExitSig, sizeof(kCraftingOnExitSig), reinterpret_cast<void*>(&HookCraftingOnExit),
         reinterpret_cast<void**>(&g_origCraftingOnExit), "CampsiteCrafting::OnExit"},
        {kRvaPopulateSkills, kPopulateSkillsSig, sizeof(kPopulateSkillsSig), reinterpret_cast<void*>(&HookPopulateSkills),
         reinterpret_cast<void**>(&g_origPopulateSkills), "CampsiteCrafting::PopulateSkills"},
        {kRvaCraftingOnUiSelect, kCraftingOnUiSelectSig, sizeof(kCraftingOnUiSelectSig), reinterpret_cast<void*>(&HookCraftingOnUiSelect),
         reinterpret_cast<void**>(&g_origCraftingOnUiSelect), "CampsiteCrafting::OnUiSelect"},
        {kRvaUpdateInformation, kUpdateInformationSig, sizeof(kUpdateInformationSig), reinterpret_cast<void*>(&HookUpdateInformation),
         reinterpret_cast<void**>(&g_origUpdateInformation), "CampsiteCrafting::UpdateInformationWidgetAndButtonLegend"},
        {kRvaSetupNavigationMenu, kSetupNavigationMenuSig, sizeof(kSetupNavigationMenuSig), reinterpret_cast<void*>(&HookSetupNavigationMenu),
         reinterpret_cast<void**>(&g_origSetupNavigationMenu), "CampsiteCrafting::SetupNavigationMenu"},
        {kRvaInvokeStateMovie, kInvokeStateMovieSig, sizeof(kInvokeStateMovieSig), reinterpret_cast<void*>(&HookInvokeStateMovie),
         reinterpret_cast<void**>(&g_origInvokeStateMovie), "UiMenuStateBase::InvokeStateMovie"},
        {kRvaRegisterNavItems, kRegisterNavItemsSig, sizeof(kRegisterNavItemsSig), reinterpret_cast<void*>(&HookRegisterNavItems),
         reinterpret_cast<void**>(&g_origRegisterNavItems), "UiMenuStateGeneric::RegisterNavItems"},
    };
    for (const auto& h : hooks)
        if (!GameBuild::VerifyBytes(h.rva, h.sig, h.size)) {
            Log::Writef("CustomOutfits: custom parts: %s signature mismatch; the Inventory is left as it is", h.name);
            return;
        }
    int created = 0;
    for (const auto& h : hooks) {
        if (!CreateHook(h.rva, h.sig, h.size, h.detour, h.original, h.name)) break;
        ++created;
    }
    if (created == static_cast<int>(sizeof(hooks) / sizeof(hooks[0])) &&
        MH_CreateHook(reinterpret_cast<void*>(GameBuild::Base() + kRvaSetHeaderIcon), reinterpret_cast<void*>(&HookSetHeaderIcon),
                      reinterpret_cast<void**>(&g_origSetHeaderIcon)) == MH_OK) {
        g_partsEnabled = true;
        Log::Detail("CustomOutfits: custom parts: the Inventory gets a \"Custom Parts\" tile while an outfit with toggles is worn");
        return;
    }
    for (int i = 0; i < created; ++i) MH_RemoveHook(reinterpret_cast<void*>(GameBuild::Base() + hooks[i].rva));
    Log::Write("CustomOutfits: custom parts: hooks could not be created; the Inventory is left as it is");
}

// ---- The Outfits screen's parts: the information panel's empty COST / INVENTORY table as ITEM / STATE ---------------
// The campsite's Gear menu (UiMenuStateCampsiteGear) shows the highlighted outfit in an information panel
// (Basecamp\Widget_Equipment_Loadout_Information, clip Information_Outfits) whose table (AddResourceIcon: rows
// resource_<i>_mc and Wallet_<i>_mc, three on its background) lists a recipe's costs only; outfits are owned, so it
// stays empty. For an outfit with toggles it lists them: the part's title, SHOWN or HIDDEN. Y ("Edit parts") enters the
// table: Up/Down pick a row, Select shows or hides it, Back or Y leave it; a click on a row toggles it.
constexpr std::uintptr_t kRvaGearUpdateInformation = 0x0095AF20ull;  // UiMenuStateCampsiteGear::UpdateInformationWidget...
constexpr std::uintptr_t kRvaGearButtonLegend = 0x00956120ull;       // UiMenuStateCampsiteGear::UpdateButtonLegend
constexpr std::uintptr_t kRvaGearInputPressed = 0x0091A640ull;       // UiMenuStateCampsiteGear::OnInputActionPressed
constexpr std::uintptr_t kRvaGearInputOn = 0x00917610ull;            // UiMenuStateCampsiteGear::OnInputActionOn
constexpr std::uintptr_t kRvaGearProcess = 0x0093C140ull;            // UiMenuStateCampsiteGear::Process
constexpr std::uintptr_t kRvaGearOnExit = 0x00912610ull;             // UiMenuStateCampsiteGear::OnExit
constexpr std::uintptr_t kRvaGearOnUiSelect = 0x00930020ull;         // UiMenuStateCampsiteGear::OnUiSelect (the list's)
constexpr std::uintptr_t kRvaGetLocalString = 0x008B3D30ull;         // MenuManager::GetLocalString(const char* key)
constexpr std::uintptr_t kRvaGetContextbar = 0x008B38C0ull;          // UiMenuStateGeneric::GetContextbarWidget
constexpr std::uintptr_t kRvaClearItems = 0x009799D0ull;             // UiWidgetContextbar::ClearItems
constexpr std::uintptr_t kRvaAddItem = 0x00973000ull;                // UiWidgetContextbar::AddItem(int, const char*, ...)
constexpr std::uintptr_t kRvaGfxSetMember = 0x00AF6A10ull;           // GFxValue::ObjectInterface::SetMember
constexpr std::uintptr_t kRvaWidgetGetVariable = 0x0098A680ull;      // UiWidgetScaleform::GetVariable
constexpr std::uintptr_t kRvaGearCategoryTypes = 0x00DB4088ull;      // int[6]: what the Gear menu's categories are
constexpr std::uint8_t kGearUpdateInformationSig[] = {0x48, 0x89, 0x5C, 0x24, 0x18, 0x55, 0x56, 0x57, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57};
constexpr std::uint8_t kGearButtonLegendSig[] = {0x40, 0x53, 0x57, 0x48, 0x83, 0xEC, 0x38, 0x48, 0x8B, 0xD9, 0xE8, 0x91, 0xD7, 0xF5, 0xFF, 0x48};
constexpr std::uint8_t kGearInputPressedSig[] = {0x40, 0x55, 0x53, 0x57, 0x41, 0x56, 0x48, 0x8B, 0xEC, 0x48, 0x83, 0xEC, 0x58, 0x80, 0xB9, 0x38};
constexpr std::uint8_t kGearInputOnSig[] = {0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x74, 0x24, 0x10, 0x57, 0x48, 0x83, 0xEC, 0x20, 0x80};
constexpr std::uint8_t kGearProcessSig[] = {0x40, 0x55, 0x56, 0x57, 0x48, 0x8B, 0xEC, 0x48, 0x83, 0xEC, 0x50, 0x80, 0xB9, 0x38, 0x02, 0x00};
constexpr std::uint8_t kGearOnUiSelectSig[] = {0x48, 0x89, 0x5C, 0x24, 0x18, 0x48, 0x89, 0x6C, 0x24, 0x20, 0x57, 0x48, 0x83, 0xEC, 0x40, 0x48, 0x63, 0x81, 0xF4, 0x01};
constexpr std::uint8_t kGearOnExitSig[] = {0x40, 0x53, 0x57, 0x41, 0x56, 0x41, 0x57, 0x48, 0x83, 0xEC, 0x48, 0x4D, 0x8B, 0xF0, 0x48, 0x8B};
constexpr std::uint8_t kGetLocalStringSig[] = {0x40, 0x55, 0x48, 0x83, 0xEC, 0x30, 0x48, 0x8D, 0x6C, 0x24, 0x20, 0x48, 0x89, 0x5D, 0x20, 0x48};
constexpr std::uint8_t kGetContextbarSig[] = {0x8B, 0x91, 0x1C, 0x01, 0x00, 0x00, 0x83, 0xFA, 0xFF, 0x75, 0x03, 0x33, 0xC0, 0xC3, 0x48, 0x8B};
constexpr std::uint8_t kClearItemsSig[] = {0x40, 0x53, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x8B, 0xD9, 0x48, 0x81, 0xC1, 0xE8, 0x01, 0x00, 0x00};
constexpr std::uint8_t kAddItemSig[] = {0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x6C, 0x24, 0x18, 0x56, 0x57, 0x41, 0x56, 0x48, 0x83};
constexpr std::uint8_t kGfxSetMemberSig[] = {0x48, 0x89, 0x6C, 0x24, 0x18, 0x56, 0x57, 0x41, 0x56, 0x48, 0x83, 0xEC, 0x50, 0x80, 0xBC, 0x24};
constexpr std::uint8_t kWidgetGetVariableSig[] = {0x48, 0x8B, 0x89, 0x98, 0x00, 0x00, 0x00, 0x48, 0x85, 0xC9, 0x75, 0x03, 0x32, 0xC0, 0xC3, 0x48};

constexpr std::ptrdiff_t kGearPopupWidget = 0x154;    // int: an open popup's widget (-1: none; the game takes Select only)
constexpr std::ptrdiff_t kGearCount = 0x1F0;          // int: the list's outfit rows (after them: broken parts); row n
                                                      // is sorted outfit n (the rows themselves: OutfitMenuRows)
constexpr std::ptrdiff_t kGearCategory = 0x1F4;       // int: an index into the category types
constexpr std::ptrdiff_t kGearInfoWidget = 0x230;     // int: the Outfits information panel's widget ID
constexpr std::ptrdiff_t kGearInputBlocked = 0x238;   // u8
constexpr std::ptrdiff_t kGearEntries = 0x280;        // 32 bytes each: +0 dtp outfit, +0x18 its outfit list index
constexpr std::ptrdiff_t kWidgetMovie = 0x98;         // UiWidgetScaleform: its GFxMovie
constexpr std::ptrdiff_t kWidgetName = 0xA8;          // char[]: its clip under _root
constexpr std::ptrdiff_t kContextbarCount = 0x1E8;    // UiWidgetContextbar: its items
constexpr int kGearCategoryCount = 6;
constexpr int kOutfitsCategoryType = 14;
constexpr int kActionUp[] = {0x13B, 0x13D, 0x13F};     // INPUTACTION_UI_Up, _RS, _DPad
constexpr int kActionDown[] = {0x141, 0x143, 0x145};   // INPUTACTION_UI_Down, _RS, _DPad
constexpr int kActionSelect = 0x153;
constexpr int kActionClose = 0x154;
constexpr int kActionY = 0x166;
constexpr int kGearRows = 6;                          // rows shown at most (two a background row past three parts)
constexpr int kHeaderDepths[2] = {46 - 16384, 47 - 16384};   // its COST and INVENTORY titles (unnamed text fields)
constexpr int kSoundNavigate = 0, kSoundSelect = 3, kSoundBack = 5;   // UiMenuStateCampsiteGear::PlaySound

using GfxSetMemberFn = bool(__fastcall*)(void*, void*, const char*, const GfxValue*, bool);
using WidgetGetVariableFn = bool(__fastcall*)(void*, GfxValue*, const char*, const char*);   // (path, "" as the game passes)
using GetContextbarFn = void*(__fastcall*)(void*);
using ClearItemsFn = void(__fastcall*)(void*);
using AddItemFn = void(__fastcall*)(void*, int, const char*, std::uintptr_t, std::uint8_t, std::uint8_t);
using GearInputFn = void(__fastcall*)(void*, int, std::uintptr_t);
using GearProcessFn = std::uintptr_t(__fastcall*)(void*, float, void*, void*);
using GetLocalStringFn = const char*(__fastcall*)(void*, const char*);
using PlaySoundFn = void(__fastcall*)(void*, int);

StateFn g_origGearUpdateInformation = nullptr, g_origGearButtonLegend = nullptr, g_origGearOnExit = nullptr,
        g_origGearOnUiSelect = nullptr;
GearInputFn g_origGearInputPressed = nullptr, g_origGearInputOn = nullptr;
GearProcessFn g_origGearProcess = nullptr;
GetLocalStringFn g_origGetLocalString = nullptr;

// The context bar's labels: localization keys, answered by the GetLocalString hook (shown as they are otherwise).
constexpr const char* kLabelEdit = "Edit parts";
constexpr const char* kLabelShow = "Show part";
constexpr const char* kLabelHide = "Hide part";
constexpr const char* kLabelDone = "Done";

constexpr std::uintptr_t kRvaIsMouseKeyboardStatus = 0x0066A660ull;   // CSCCF_Status_IsMouseKeyboard (reads the flag)
constexpr std::uint8_t kIsMouseKeyboardStatusSig[] = {0x41, 0x8B, 0x10, 0x0F, 0xB6, 0x0D, 0x16, 0xC9, 0x94, 0x00, 0x41, 0x0F, 0xB7, 0x40, 0x0C, 0x83};
constexpr std::uintptr_t kRvaUsingMouseKeyboard = 0x00FB6F80ull;      // u8: mouse & keyboard in use (InputDevice::Update)

constexpr int kBackgroundRows = 3;           // the table's background rows (Wallet_Hatching_0..2_mc)
constexpr double kCompactScale = 0.9;        // more than three parts: two rows a background row, a little smaller
constexpr int kDividerDepth = 32 - 16384;    // the line between the two columns (an unnamed clip)

struct GearParts {
    void* state = nullptr;      // the Gear menu the panel belongs to (game thread)
    int highlight = -1;         // its highlighted item
    int config = -1;            // the highlighted outfit (with toggles; -1: none)
    bool editing = false;       // in the table (Y)
    int selected = 0;           // the part chosen in it
    int first = 0;              // the part in the table's first row
    int rows = 0;               // the rows shown
    bool compact = false;       // six rows
    bool touched = false;       // rows changed by us (moved, their icon hidden, their text widened)
    int headerMode = 0;         // 0: the game's titles, 1: ITEM / STATE, 2: NO TOGGLES
    std::string headers[2];     // the game's titles
    double headerWidth = 0.0;   // the first title's width
    bool headersSaved = false;
    std::uint32_t red = 0;      // the table's red (its rows' frame 2), once read
    bool redKnown = false;
    std::uint32_t stateColor = 0;   // the second column's own color
    bool stateColorKnown = false;
    int hover = -1;             // the part under the mouse (-1: none)
    double rowCenter0 = 0.0;    // the rows' places in asset_mc (the first row's middle, the spacing, the columns)
    double rowStep = 0.0;
    double tableLeft = 219.65, tableRight = 456.6;
    int mouseLogs = 0;
    int frame = 0;              // Process calls (one Up / Down a frame: the D-pad sends two actions)
    int navFrame = -1;
    bool mouseDown = false;
    bool keyDown = false;
    bool toggleKeyDown = false;
    std::uint64_t lastToggle = 0;   // (one toggle per 150 ms: a key and an action for one press)
    int key = 'T';              // [Outfits] OutfitsScreenPartsKey: enters / leaves the table on the keyboard
    std::string editKeyLabel = "Edit parts [T]";
    int clicksLogged = 0;
    int loggedConfig = -1;
    bool loggedHeaders = false;
    bool loggedPanel = false;
    bool loggedSeparators = false;
    bool mouseKeyboardKnown = false;   // the flag's reader matched
};
GearParts g_gear;

bool GfxIsDisplay(const GfxValue& value) { return (value.type & 0x8F) == 8; }
GfxValue GfxNumber(double number) {
    GfxValue value;
    value.type = 3;
    value.number = number;
    return value;
}
GfxValue GfxString(const char* text) {
    GfxValue value;
    value.type = 4;
    value.string = text;
    return value;
}
GfxValue GfxBool(bool on) {
    GfxValue value;
    value.type = 2;
    value.boolean = on;
    return value;
}
bool GfxGet(const GfxValue& object, const char* name, GfxValue& out) {
    if (!object.iface) return false;
    return Game<GfxGetMemberFn>(kRvaGfxGetMember)(object.iface, object.data, name, &out, GfxIsDisplay(object));
}
bool GfxSet(const GfxValue& object, const char* name, const GfxValue& value) {
    if (!object.iface) return false;
    return Game<GfxSetMemberFn>(kRvaGfxSetMember)(object.iface, object.data, name, &value, GfxIsDisplay(object));
}
bool GfxCall(const GfxValue& object, const char* method, const GfxValue* args, std::size_t count, GfxValue* result) {
    if (!object.iface) return false;
    return Game<GfxInvokeFn>(kRvaGfxInvoke)(object.iface, object.data, result, method, args, count, GfxIsDisplay(object));
}
double GfxGetNumber(const GfxValue& object, const char* name, double fallback) {
    GfxValue value;
    if (!GfxGet(object, name, value)) return fallback;
    const double number = (value.type & 0x8F) == 3 ? value.number : fallback;
    GfxRelease(value);
    return number;
}
bool GfxGetString(const GfxValue& object, const char* name, std::string& out) {
    GfxValue value;
    if (!GfxGet(object, name, value)) return false;
    const bool ok = (value.type & 0x8F) == 4 && value.string;
    if (ok) out = value.string;
    GfxRelease(value);
    return ok;
}
// A child clip (a display object), released by the caller.
bool GfxClip(const GfxValue& parent, const char* name, GfxValue& out) {
    if (!GfxGet(parent, name, out)) return false;
    if (out.iface && ((out.type & 0x8F) == 8 || (out.type & 0x8F) == 6)) return true;
    GfxRelease(out);
    return false;
}

// A variable of the widget's movie: a full path ("_root....").
bool MovieGet(void* widget, const char* path, GfxValue& out) {
    if (!widget || !At<void*>(widget, kWidgetMovie)) return false;
    return Game<WidgetGetVariableFn>(kRvaWidgetGetVariable)(widget, &out, path, "");
}
// The widget's own clip's child ("asset_mc").
bool WidgetClip(void* widget, const char* child, GfxValue& out, bool quiet = false) {
    if (!widget) return false;
    const char* name = static_cast<const char*>(widget) + kWidgetName;
    if (!std::memchr(name, 0, 128) || !*name) return false;
    char path[256];
    std::snprintf(path, sizeof(path), "_root.%s.%s", name, child);
    const bool got = MovieGet(widget, path, out);
    if (got && out.iface && ((out.type & 0x8F) == 8 || (out.type & 0x8F) == 6)) {
        static bool logged = false;
        if (!logged && !quiet) Log::Detailf("CustomOutfits: outfit parts: the information panel is %s", path);
        logged = logged || !quiet;
        return true;
    }
    if (!g_gear.loggedPanel && !quiet)
        Log::Detailf("CustomOutfits: outfit parts: %s: %s (type %u)", path, got ? "not a clip" : "not found", out.type);
    g_gear.loggedPanel = g_gear.loggedPanel || !quiet;
    GfxRelease(out);
    return false;
}

bool GearOnOutfits(void* state) {
    const int category = At<std::int32_t>(state, kGearCategory);
    return category >= 0 && category < kGearCategoryCount &&
           Game<const std::int32_t*>(kRvaGearCategoryTypes)[category] == kOutfitsCategoryType;
}

int ConfigForListIndex(unsigned int index) {
    std::lock_guard<std::mutex> lock(g_appliedMutex);
    for (const auto& a : g_applied)
        if (a.listIndex == index) return static_cast<int>(a.config);
    for (const auto& r : g_replaced)
        if (r.listIndex == index) return static_cast<int>(r.config);
    return -1;
}

// The highlighted outfit, when it is one of ours with toggles (-1: none).
int GearConfig(void* state) {
    if (!GearOnOutfits(state)) return -1;
    const int item = At<std::int32_t>(state, kStateSelectedIndex);
    if (item < 0 || item >= At<std::int32_t>(state, kGearCount)) return -1;   // (or a broken part)
    std::uint8_t* entries = At<std::uint8_t*>(state, kGearEntries);
    if (!entries) return -1;
    // (UiMenuStateCampsiteGear::PopulateSkills gives the outfits the first rows, in their sorted order: row n is
    // entry n. 0.9.400 and before read the row from the menu's 32-row array, wrong from row 32.)
    const unsigned int entry = static_cast<unsigned int>(item);
    const std::int32_t listIndex = At<std::int32_t>(entries + entry * 32, 0x18);
    if (listIndex < 0) return -1;
    const int config = ConfigForListIndex(static_cast<unsigned int>(listIndex));
    if (config < 0 || static_cast<std::size_t>(config) >= g_configs.size() || g_configs[static_cast<std::size_t>(config)].menuToggles.empty())
        return -1;   // (v0.9.604: the menu's toggles: none following a holster)
    return config;
}

int GearPartCount() {
    if (g_gear.config < 0) return 0;
    return static_cast<int>(std::min<std::size_t>(g_configs[static_cast<std::size_t>(g_gear.config)].menuToggles.size(), kMaxParts));
}
bool GearPartOn(int part) {
    if (part < 0 || part >= GearPartCount()) return false;
    std::lock_guard<std::mutex> lock(g_toggleMutex);
    const auto& config = g_configs[static_cast<std::size_t>(g_gear.config)];
    return config.toggles[config.menuToggles[static_cast<std::size_t>(part)]].on;
}

void* GearWidget(void* state) {
    void* manager = Game<WidgetManagerGetFn>(kRvaWidgetManagerGet)();
    return manager ? Game<GetWidgetFn>(kRvaGetWidget)(manager, At<std::int32_t>(state, kGearInfoWidget)) : nullptr;
}

void GearPlaySound(void* state, int sound) {
    reinterpret_cast<PlaySoundFn>((*reinterpret_cast<void***>(state))[0xC0 / 8])(state, sound);
}

bool GearRowClip(const GfxValue& asset, const char* prefix, int row, GfxValue& out) {
    char name[32];
    std::snprintf(name, sizeof(name), "%s%d_mc", prefix, row);
    return GfxClip(asset, name, out);
}

std::string HtmlEscape(const std::string& text) {
    std::string out;
    for (const char c : text) {
        if (c == '&') out += "&amp;";
        else if (c == '<') out += "&lt;";
        else if (c == '>') out += "&gt;";
        else out += c;
    }
    return out;
}

bool UsingMouseKeyboard() {
    return g_gear.mouseKeyboardKnown && *Game<const std::uint8_t*>(kRvaUsingMouseKeyboard) != 0;
}

// A row's first column (an EquipmentResource: its icon and a number): the part's title over the whole column (the
// table's first title spans asset_mc x 224.7..340.7), centered, its icon hidden. The field's old place and html flag
// are kept in the row's fwX / fwW / fwHtml to put back.
constexpr double kItemColumnLeft = 226.0, kItemColumnWidth = 112.0;
void PlaceItemText(const GfxValue& resource, const std::string& title) {
    GfxValue cost, icon;
    if (GfxClip(resource, "icon_mc", icon)) GfxSet(icon, "_visible", GfxBool(false));
    if (GfxClip(resource, "cost", cost)) {
        GfxValue saved;
        const bool kept = GfxGet(resource, "fwX", saved) && (saved.type & 0x8F) == 3;
        GfxRelease(saved);
        if (!kept) {
            GfxValue html;
            GfxGet(cost, "html", html);
            GfxSet(resource, "fwHtml", GfxBool((html.type & 0x8F) == 2 && html.boolean));
            GfxRelease(html);
            GfxSet(resource, "fwX", GfxNumber(GfxGetNumber(cost, "_x", 0.0)));
            GfxSet(resource, "fwW", GfxNumber(GfxGetNumber(cost, "_width", 0.0)));
        }
        const double x = GfxGetNumber(resource, "_x", kItemColumnLeft);
        const double scale = GfxGetNumber(resource, "_xscale", 100.0) / 100.0;
        if (scale > 0.01) {
            GfxSet(cost, "_x", GfxNumber((kItemColumnLeft - x) / scale));
            GfxSet(cost, "_width", GfxNumber(kItemColumnWidth / scale));
        }
        GfxSet(cost, "html", GfxBool(true));
        const std::string html = "<p align=\"center\">" + HtmlEscape(title) + "</p>";
        GfxSet(cost, "htmlText", GfxString(html.c_str()));
    }
    GfxRelease(cost);
    GfxRelease(icon);
}
void RestoreItemText(const GfxValue& resource) {
    GfxValue saved;
    const bool kept = GfxGet(resource, "fwX", saved) && (saved.type & 0x8F) == 3;
    const double x = kept ? saved.number : 0.0;
    GfxRelease(saved);
    GfxValue cost, icon, html;
    if (kept && GfxClip(resource, "cost", cost)) {
        GfxSet(cost, "_x", GfxNumber(x));
        const double width = GfxGetNumber(resource, "fwW", 0.0);
        if (width > 0.0) GfxSet(cost, "_width", GfxNumber(width));
        if (GfxGet(resource, "fwHtml", html) && (html.type & 0x8F) == 2) GfxSet(cost, "html", html);
        GfxSet(resource, "fwX", GfxValue());
    }
    if (GfxClip(resource, "icon_mc", icon)) GfxSet(icon, "_visible", GfxBool(true));
    GfxSet(resource, "_alpha", GfxNumber(100.0));
    GfxRelease(html);
    GfxRelease(icon);
    GfxRelease(cost);
}

// A row clip's text color (its "cost" field).
bool GetTextColor(const GfxValue& row, std::uint32_t& color) {
    GfxValue cost;
    bool ok = false;
    if (GfxClip(row, "cost", cost)) {
        GfxValue value;
        if (GfxGet(cost, "textColor", value) && (value.type & 0x8F) == 3) {
            color = static_cast<std::uint32_t>(value.number);
            ok = true;
        }
        GfxRelease(value);
    }
    GfxRelease(cost);
    return ok;
}
void SetTextColor(const GfxValue& row, std::uint32_t color) {
    GfxValue cost;
    if (GfxClip(row, "cost", cost)) GfxSet(cost, "textColor", GfxNumber(static_cast<double>(color)));
    GfxRelease(cost);
}

// The place and scale of the panel's own first row (rows 1.. are placed from it): kept in fwY / fwXS / fwYS / fwH
// before it is moved, to place the rows from and to put back.
struct RowBase {
    double y = 0.0, xscale = 100.0, yscale = 100.0, height = 0.0;
};
RowBase RowBaseOf(const GfxValue& row, const GfxValue& wallet0) {
    RowBase base;
    GfxValue saved;
    const bool kept = GfxGet(row, "fwY", saved) && (saved.type & 0x8F) == 3;
    GfxRelease(saved);
    if (!kept) {
        GfxSet(row, "fwY", GfxNumber(GfxGetNumber(row, "_y", 0.0)));
        GfxSet(row, "fwXS", GfxNumber(GfxGetNumber(row, "_xscale", 100.0)));
        GfxSet(row, "fwYS", GfxNumber(GfxGetNumber(row, "_yscale", 100.0)));
        GfxSet(row, "fwH", GfxNumber(GfxGetNumber(wallet0, "_height", 0.0)));
    }
    base.y = GfxGetNumber(row, "fwY", 0.0);
    base.xscale = GfxGetNumber(row, "fwXS", 100.0);
    base.yscale = GfxGetNumber(row, "fwYS", 100.0);
    base.height = GfxGetNumber(row, "fwH", 0.0);
    return base;
}
void RestoreRowBase(const GfxValue& row) {
    GfxValue saved;
    if (GfxGet(row, "fwY", saved) && (saved.type & 0x8F) == 3) {
        GfxSet(row, "_y", GfxNumber(saved.number));
        GfxSet(row, "_xscale", GfxNumber(GfxGetNumber(row, "fwXS", 100.0)));
        GfxSet(row, "_yscale", GfxNumber(GfxGetNumber(row, "fwYS", 100.0)));
        GfxSet(row, "fwY", GfxValue());
    }
    GfxRelease(saved);
}

// The table's column lines (unnamed clips at depths 29, 32 and 35: left edge, between the columns, right edge). Our
// rows go above them (rows 1.. are attached at the top, row 0 is raised), and a row's clip covers the lines where it
// lies, so the lines are raised above the rows; they are kept as asset_mc.fwCol0..2 first (their depths change).
constexpr int kColumnLineDepths[3] = {29 - 16384, 32 - 16384, 35 - 16384};
bool ColumnLine(const GfxValue& asset, int index, GfxValue& out) {
    char name[16];
    std::snprintf(name, sizeof(name), "fwCol%d", index);
    if (GfxClip(asset, name, out)) return true;
    GfxValue depth = GfxNumber(kColumnLineDepths[index]);
    if (GfxCall(asset, "getInstanceAtDepth", &depth, 1, &out) && out.iface && GfxIsDisplay(out)) {
        GfxSet(asset, name, out);
        return true;
    }
    GfxRelease(out);
    return false;
}
double ClipDepth(const GfxValue& clip) {
    GfxValue depth;
    double d = -1e9;
    if (GfxCall(clip, "getDepth", nullptr, 0, &depth) && (depth.type & 0x8F) == 3) d = depth.number;
    GfxRelease(depth);
    return d;
}
void RaiseColumnLines(const GfxValue& asset, double topRowDepth) {
    for (int i = 0; i < 3; ++i) {
        GfxValue line;
        if (ColumnLine(asset, i, line) && ClipDepth(line) < topRowDepth) {
            GfxValue next;
            if (GfxCall(asset, "getNextHighestDepth", nullptr, 0, &next) && (next.type & 0x8F) == 3) {
                GfxValue to = GfxNumber(next.number);
                GfxCall(line, "swapDepths", &to, 1, nullptr);
                topRowDepth = std::max(topRowDepth, next.number);
            }
            GfxRelease(next);
        }
        GfxRelease(line);
    }
}

// The table's titles: 0 the game's, 1 ITEM / STATE (item: the first one's text), 2 one title over both, NO TOGGLES
// (the line between the columns hidden).
void SetGearHeaders(const GfxValue& asset, int mode, const char* item) {
    if (mode == g_gear.headerMode && mode != 1) return;
    GfxValue fields[2], divider;
    bool found = true;
    for (int i = 0; i < 2; ++i) {
        GfxValue depth = GfxNumber(kHeaderDepths[i]);
        found = GfxCall(asset, "getInstanceAtDepth", &depth, 1, &fields[i]) && found;
        if (!fields[i].iface || ((fields[i].type & 0x8F) != 8 && (fields[i].type & 0x8F) != 6)) found = false;
    }
    ColumnLine(asset, 1, divider);
    if (!found) {
        if (!g_gear.loggedHeaders)
            Log::Writef("CustomOutfits: outfit parts: the table's titles were not found (types %u, %u); they keep their text",
                        fields[0].type, fields[1].type);
        g_gear.loggedHeaders = true;
    } else {
        if (!g_gear.headersSaved) {
            g_gear.headersSaved = GfxGetString(fields[0], "text", g_gear.headers[0]) && GfxGetString(fields[1], "text", g_gear.headers[1]);
            g_gear.headerWidth = GfxGetNumber(fields[0], "_width", 0.0);
        }
        if (mode == 2) {
            const double left = GfxGetNumber(fields[0], "_x", 0.0);
            const double right = GfxGetNumber(fields[1], "_x", 0.0) + GfxGetNumber(fields[1], "_width", 0.0);
            if (right > left) GfxSet(fields[0], "_width", GfxNumber(right - left));
            GfxSet(fields[0], "text", GfxString("NO TOGGLES"));
            GfxSet(fields[1], "text", GfxString(""));
        } else {
            if (g_gear.headerWidth > 0.0) GfxSet(fields[0], "_width", GfxNumber(g_gear.headerWidth));
            if (mode == 1) {
                GfxSet(fields[0], "text", GfxString(item));
                GfxSet(fields[1], "text", GfxString("STATE"));
            } else if (g_gear.headersSaved) {
                GfxSet(fields[0], "text", GfxString(g_gear.headers[0].c_str()));
                GfxSet(fields[1], "text", GfxString(g_gear.headers[1].c_str()));
            }
        }
        if (divider.iface && ((divider.type & 0x8F) == 8 || (divider.type & 0x8F) == 6))
            GfxSet(divider, "_visible", GfxBool(mode != 2));
        if (!g_gear.loggedHeaders) Log::Detail("CustomOutfits: outfit parts: the table's titles found");
        g_gear.loggedHeaders = true;
        g_gear.headerMode = mode;
    }
    GfxRelease(divider);
    GfxRelease(fields[1]);
    GfxRelease(fields[0]);
}

// Past three parts: three more lines between the rows, drawn in asset_mc.fwLines (thin, as the table's own) while the
// rows are compact. The table's lines (clips at depths 40 / 43) show a texture (MissionSubLine); their copies made with
// duplicateMovieClip (fwSep0..2 until 0.9.397) showed as black bands 32 high (the texture's height) across the table,
// the first one over the first row. Returns half the lines' spacing (0: the lines were not found).
constexpr int kSeparatorDepths[2] = {40 - 16384, 43 - 16384};
constexpr int kDrawnLinesDepth = 899;
constexpr double kDrawnLineWidth = 1.2, kDrawnLineAlpha = 20.0;   // (as the table's lines look: about 20% white)
double SetSeparators(const GfxValue& asset, bool compact) {
    GfxValue lines[2];
    double half = 0.0;
    for (int i = 0; i < 2; ++i) {
        GfxValue depth = GfxNumber(kSeparatorDepths[i]);
        GfxCall(asset, "getInstanceAtDepth", &depth, 1, &lines[i]);
    }
    const bool found = lines[0].iface && lines[1].iface && GfxIsDisplay(lines[0]) && GfxIsDisplay(lines[1]);
    GfxValue drawn;
    if (!GfxClip(asset, "fwLines", drawn) && compact && found) {
        GfxValue args[2] = {GfxString("fwLines"), GfxNumber(kDrawnLinesDepth)};
        GfxValue made;
        GfxCall(asset, "createEmptyMovieClip", args, 2, &made);
        GfxRelease(made);
        GfxClip(asset, "fwLines", drawn);
    }
    if (drawn.iface) GfxCall(drawn, "clear", nullptr, 0, nullptr);
    if (found) {
        const double y0 = GfxGetNumber(lines[0], "_y", 0.0), y1 = GfxGetNumber(lines[1], "_y", 0.0);
        half = (y1 - y0) / 2.0;
        if (compact && drawn.iface) {
            const double places[3] = {y0 - half, y0 + half, y1 + half};
            GfxValue style[3] = {GfxNumber(kDrawnLineWidth), GfxNumber(static_cast<double>(0xFFFFFF)), GfxNumber(kDrawnLineAlpha)};
            GfxCall(drawn, "lineStyle", style, 3, nullptr);
            for (const double y : places) {
                GfxValue from[2] = {GfxNumber(g_gear.tableLeft), GfxNumber(y)}, to[2] = {GfxNumber(g_gear.tableRight), GfxNumber(y)};
                GfxCall(drawn, "moveTo", from, 2, nullptr);
                GfxCall(drawn, "lineTo", to, 2, nullptr);
            }
        } else if (compact && !g_gear.loggedSeparators) {
            Log::Write("CustomOutfits: outfit parts: the table's lines could not be drawn");
            g_gear.loggedSeparators = true;
        }
    } else if (compact && !g_gear.loggedSeparators) {
        Log::Write("CustomOutfits: outfit parts: the table's lines were not found");
        g_gear.loggedSeparators = true;
    }
    GfxRelease(drawn);
    GfxRelease(lines[1]);
    GfxRelease(lines[0]);
    return half;
}

// The panel's crossfade (OnHighlight -> DuplicateData: a fresh copy of asset_mc, transition_mc, over it, with the
// rows' texts and frames copied but not our changes): its table hidden, so asset_mc's shows through at once.
void HideTransitionTable(void* widget) {
    GfxValue transition;
    if (!WidgetClip(widget, "transition_mc", transition, true)) return;
    auto hide = [&](const GfxValue& clip) {
        if (clip.iface && ((clip.type & 0x8F) == 8 || (clip.type & 0x8F) == 6)) GfxSet(clip, "_visible", GfxBool(false));
    };
    for (int r = 0; r < 7; ++r)
        for (const char* prefix : {"resource_", "Wallet_"}) {
            GfxValue row;
            if (GearRowClip(transition, prefix, r, row)) hide(row);
            GfxRelease(row);
        }
    for (const char* name : {"Wallet_Hatching_0_mc", "Wallet_Hatching_1_mc", "Wallet_Hatching_2_mc", "Hatching_NoReq"}) {
        GfxValue clip;
        if (GfxClip(transition, name, clip)) hide(clip);
        GfxRelease(clip);
    }
    for (const int depth : {kHeaderDepths[0], kHeaderDepths[1], kDividerDepth, kSeparatorDepths[0], kSeparatorDepths[1]}) {
        GfxValue at = GfxNumber(depth), clip;
        GfxCall(transition, "getInstanceAtDepth", &at, 1, &clip);
        hide(clip);
        GfxRelease(clip);
    }
    GfxRelease(transition);
}

bool GameRowsShown(const GfxValue& asset) {
    GfxValue row, visible;
    bool shown = false;
    if (GearRowClip(asset, "resource_", 0, row) && GfxGet(row, "_visible", visible))
        shown = (visible.type & 0x8F) == 2 && visible.boolean;
    GfxRelease(visible);
    GfxRelease(row);
    return shown;
}

// Our changes to the rows put back (the table is the game's again).
void RestoreGearRows(const GfxValue& asset) {
    if (!g_gear.touched) return;
    for (int r = 0; r < kGearRows; ++r) {
        GfxValue resource, wallet;
        if (GearRowClip(asset, "resource_", r, resource)) RestoreItemText(resource);
        if (GearRowClip(asset, "Wallet_", r, wallet)) {
            GfxSet(wallet, "_alpha", GfxNumber(100.0));
            if (g_gear.stateColorKnown) SetTextColor(wallet, g_gear.stateColor);
        }
        if (r == 0) {
            RestoreRowBase(resource);
            RestoreRowBase(wallet);
        }
        GfxRelease(wallet);
        GfxRelease(resource);
    }
    g_gear.touched = false;
}

// A row's brightness: dimmed while the table is not being edited (it can still be clicked), the part chosen while editing
// and the one under the mouse at full brightness.
constexpr double kRowDimmed = 45.0, kRowEditOther = 40.0;
double RowAlpha(int part) {
    if (g_gear.editing) return part == g_gear.selected ? 100.0 : kRowEditOther;   // (the mouse chooses the part there)
    return part == g_gear.hover ? 100.0 : kRowDimmed;
}
// The rows' brightness again (the mouse moved to another row).
void ApplyRowAlpha(void* widget) {
    GfxValue asset;
    if (!WidgetClip(widget, "asset_mc", asset, true)) return;
    for (int r = 0; r < g_gear.rows; ++r) {
        const GfxValue alpha = GfxNumber(RowAlpha(g_gear.first + r));
        for (const char* prefix : {"resource_", "Wallet_"}) {
            GfxValue row;
            if (GearRowClip(asset, prefix, r, row)) GfxSet(row, "_alpha", alpha);
            GfxRelease(row);
        }
    }
    GfxRelease(asset);
}

// The table: the outfit's parts (or the game's state back). reset: cleared first (the game did not just draw it).
void GearDrawRows(void* state, void* widget, bool reset) {
    GfxValue asset;
    if (!WidgetClip(widget, "asset_mc", asset)) {
        g_gear.rows = 0;
        return;
    }
    int config = g_gear.config;
    bool gameRows = false;
    if (reset) {
        Game<InvokeASFunctionFn>(kRvaInvokeASFunction)(widget, "Reset", nullptr, nullptr, 0);
    } else if (GameRowsShown(asset)) {
        if (config >= 0) Log::Detail("CustomOutfits: outfit parts: the game lists this outfit's costs; its parts are left out");
        config = -1;
        gameRows = true;
    }
    if (config < 0) {
        RestoreGearRows(asset);
        SetSeparators(asset, false);
        const int item = At<std::int32_t>(state, kStateSelectedIndex);
        const bool outfit = item >= 0 && item < At<std::int32_t>(state, kGearCount);
        SetGearHeaders(asset, outfit && !gameRows ? 2 : 0, nullptr);
        g_gear.rows = 0;
        GfxRelease(asset);
        return;
    }
    const auto& toggles = g_configs[static_cast<std::size_t>(config)].toggles;
    const int count = GearPartCount();
    const bool compact = count > kBackgroundRows;
    const int rows = std::min(compact ? kGearRows : kBackgroundRows, count);
    g_gear.selected = std::clamp(g_gear.selected, 0, count - 1);
    if (g_gear.selected < g_gear.first) g_gear.first = g_gear.selected;
    if (g_gear.selected >= g_gear.first + rows) g_gear.first = g_gear.selected - rows + 1;
    g_gear.first = std::clamp(g_gear.first, 0, count - rows);

    GfxValue resource0, wallet0;
    GearRowClip(asset, "resource_", 0, resource0);
    GearRowClip(asset, "Wallet_", 0, wallet0);
    if (!g_gear.stateColorKnown && wallet0.iface) g_gear.stateColorKnown = GetTextColor(wallet0, g_gear.stateColor);
    const RowBase resourceBase = RowBaseOf(resource0, wallet0);
    const RowBase walletBase = RowBaseOf(wallet0, wallet0);
    const double height = walletBase.height > 1.0 ? walletBase.height : 50.0;
    for (int i = 0; i < 3; i += 2) {   // (the table's left and right lines: the drawn lines' ends, the mouse's area)
        GfxValue line;
        if (ColumnLine(asset, i, line)) (i == 0 ? g_gear.tableLeft : g_gear.tableRight) = GfxGetNumber(line, "_x", i == 0 ? 219.65 : 456.6);
        GfxRelease(line);
    }
    const double half = SetSeparators(asset, compact);    // (the background rows' spacing / 2; 0: not found)
    const double step = compact ? (half > 1.0 ? half : height / 2.0) : height;
    const double shift = compact ? -step / 2.0 : 0.0;   // (row r of six: in background row r / 2)
    const double scale = compact ? kCompactScale : 1.0;
    // row 0 back where the game has it first: AddResourceIcon places rows 1.. from it
    GfxSet(resource0, "_y", GfxNumber(resourceBase.y));
    GfxSet(resource0, "_xscale", GfxNumber(resourceBase.xscale));
    GfxSet(resource0, "_yscale", GfxNumber(resourceBase.yscale));
    GfxSet(wallet0, "_y", GfxNumber(walletBase.y));
    GfxSet(wallet0, "_xscale", GfxNumber(walletBase.xscale));
    GfxSet(wallet0, "_yscale", GfxNumber(walletBase.yscale));
    for (int r = 0; r < rows; ++r) {
        const int part = g_gear.first + r;
        std::string title;
        bool on = true;
        {
            std::lock_guard<std::mutex> lock(g_toggleMutex);
            const auto& toggle = toggles[g_configs[static_cast<std::size_t>(config)].menuToggles[static_cast<std::size_t>(part)]];
            title = toggle.title.empty() ? toggle.name : toggle.title;
            on = toggle.on;
        }
        const bool readRed = !on && !g_gear.redKnown;   // (the game's red: its rows' frame 2, read once)
        GfxValue args[5];
        args[0] = GfxNumber(r);
        args[1] = GfxString(title.c_str());
        args[2] = GfxString(on ? "SHOWN" : "HIDDEN");
        args[3] = GfxString("");
        args[4] = GfxBool(readRed);
        Game<InvokeASFunctionFn>(kRvaInvokeASFunction)(widget, "AddResourceIcon", nullptr, args, 5);
        const double alpha = RowAlpha(part);
        GfxValue resource, wallet;
        if (GearRowClip(asset, "resource_", r, resource)) {
            if (readRed) {
                std::uint32_t red = 0;
                g_gear.redKnown = true;
                g_gear.red = GetTextColor(resource, red) && (!g_gear.stateColorKnown || red != g_gear.stateColor) ? red : 0xD9412Eu;
                GfxValue frame = GfxNumber(1.0);
                GfxCall(resource, "gotoAndStop", &frame, 1, nullptr);
            }
            GfxSet(resource, "_xscale", GfxNumber(resourceBase.xscale * scale));
            GfxSet(resource, "_yscale", GfxNumber(resourceBase.yscale * scale));
            GfxSet(resource, "_y", GfxNumber(resourceBase.y + shift + r * step));
            PlaceItemText(resource, title);
            GfxSet(resource, "_alpha", GfxNumber(alpha));
        }
        if (GearRowClip(asset, "Wallet_", r, wallet)) {
            GfxSet(wallet, "_xscale", GfxNumber(walletBase.xscale * scale));
            GfxSet(wallet, "_yscale", GfxNumber(walletBase.yscale * scale));
            GfxSet(wallet, "_y", GfxNumber(walletBase.y + shift + r * step));
            if (!on) SetTextColor(wallet, g_gear.redKnown ? g_gear.red : 0xD9412Eu);
            else if (g_gear.stateColorKnown) SetTextColor(wallet, g_gear.stateColor);
            GfxSet(wallet, "_alpha", GfxNumber(alpha));
        }
        GfxRelease(wallet);
        GfxRelease(resource);
    }
    // the panel's own first row is placed under the titles' dark band (depth 23, down to y 13): six rows put row 0
    // inside it, so both of its clips go to the top (once: fwTop)
    for (GfxValue* row : {&resource0, &wallet0}) {
        GfxValue top;
        const bool raised = GfxGet(*row, "fwTop", top) && (top.type & 0x8F) == 2 && top.boolean;
        GfxRelease(top);
        if (raised || !row->iface) continue;
        GfxValue depth;
        if (GfxCall(asset, "getNextHighestDepth", nullptr, 0, &depth) && (depth.type & 0x8F) == 3) {
            GfxValue to = GfxNumber(depth.number);
            GfxCall(*row, "swapDepths", &to, 1, nullptr);
            GfxSet(*row, "fwTop", GfxBool(true));
        }
        GfxRelease(depth);
    }
    // the column lines over the rows
    double topRowDepth = -1e9;
    for (int r = 0; r < rows; ++r)
        for (const char* prefix : {"resource_", "Wallet_"}) {
            GfxValue row;
            if (GearRowClip(asset, prefix, r, row)) topRowDepth = std::max(topRowDepth, ClipDepth(row));
            GfxRelease(row);
        }
    RaiseColumnLines(asset, topRowDepth);
    // the background rows: hatched while empty
    for (int k = 0; k < kBackgroundRows; ++k) {
        char name[32];
        std::snprintf(name, sizeof(name), "Wallet_Hatching_%d_mc", k);
        GfxValue hatching;
        const int firstRow = compact ? k * 2 : k;
        if (GfxClip(asset, name, hatching)) GfxSet(hatching, "_visible", GfxBool(rows <= firstRow));
        GfxRelease(hatching);
    }
    GfxRelease(wallet0);
    GfxRelease(resource0);
    g_gear.rows = rows;
    g_gear.compact = compact;
    g_gear.touched = true;
    // the rows' places for the mouse: a row's text (Wallet_mc's "cost": local y -5 .. 18) is centered 6.6 under its
    // clip's y; each row owns the band of one spacing around it (rows touch: the lines lie between them)
    g_gear.rowStep = step;
    g_gear.rowCenter0 = walletBase.y + shift + 6.6 * walletBase.yscale / 100.0 * scale;
    char item[32];
    if (count <= rows)
        std::snprintf(item, sizeof(item), "ITEM");
    else if (g_gear.editing)
        std::snprintf(item, sizeof(item), "ITEM %d/%d", g_gear.selected + 1, count);
    else
        std::snprintf(item, sizeof(item), "ITEMS: %d", count);
    SetGearHeaders(asset, 1, item);
    if (g_gear.loggedConfig != config) {
        Log::Detailf("CustomOutfits: outfit parts: %s: %d part(s) in the Outfits screen's table", 
                    g_configs[static_cast<std::size_t>(config)].key.c_str(), count);
        g_gear.loggedConfig = config;
    }
    GfxRelease(asset);
}

std::uintptr_t __fastcall HookGearButtonLegend(void* state, void* a, void* b, void* c) {
    const std::uintptr_t result = g_origGearButtonLegend(state, a, b, c);
    if (!state || state != g_gear.state || g_gear.config < 0 || At<std::int32_t>(state, kStateSelectedIndex) != g_gear.highlight)
        return result;
    void* bar = Game<GetContextbarFn>(kRvaGetContextbar)(state);
    if (!bar || At<std::uint32_t>(bar, kContextbarCount) == 0) return result;   // (the game shows no prompts now)
    if (g_gear.editing) {
        Game<ClearItemsFn>(kRvaClearItems)(bar);
        Game<AddItemFn>(kRvaAddItem)(bar, kActionSelect, GearPartOn(g_gear.selected) ? kLabelHide : kLabelShow, 2, 1, 1);
        Game<AddItemFn>(kRvaAddItem)(bar, kActionClose, kLabelDone, 2, 1, 1);
    } else {
        Game<AddItemFn>(kRvaAddItem)(bar, kActionY, UsingMouseKeyboard() ? g_gear.editKeyLabel.c_str() : kLabelEdit, 2, 1, 1);
    }
    return result;
}

// The table and the prompts again (the outfit's texts are left as they are).
void GearRefresh(void* state) {
    if (void* widget = GearWidget(state)) GearDrawRows(state, widget, true);
    HookGearButtonLegend(state, nullptr, nullptr, nullptr);
}

void GearToggle(void* state, int part) {
    if (part < 0 || part >= GearPartCount()) return;
    const std::uint64_t now = GetTickCount64();
    if (now - g_gear.lastToggle < 150) return;
    g_gear.lastToggle = now;
    auto& config = g_configs[static_cast<std::size_t>(g_gear.config)];
    const std::size_t index = config.menuToggles[static_cast<std::size_t>(part)];   // (v0.9.604: the menu's toggles)
    auto& toggle = config.toggles[index];
    bool on = true;
    {
        std::lock_guard<std::mutex> lock(g_toggleMutex);
        on = !toggle.on;
    }
    ChangeToggle(config, index, on);   // (v0.9.454: shown, the parts it is never shown with hidden)
    g_menuConfig.store(g_gear.config);
    Log::Detailf("CustomOutfits: outfit parts: %s / %s %s", config.key.c_str(), toggle.name.c_str(), on ? "shown" : "hidden");
    GearPlaySound(state, kSoundSelect);
    GearRefresh(state);
}

// The table can be used now: the panel shows the highlighted outfit's parts, no popup is open.
bool GearUsable(void* state) {
    return state && state == g_gear.state && g_gear.config >= 0 && g_gear.rows > 0 &&
           At<std::int32_t>(state, kGearPopupWidget) == -1 && At<std::int32_t>(state, kStateSelectedIndex) == g_gear.highlight;
}

void GearSetEditing(void* state, bool on) {
    g_gear.editing = on;
    Log::Detailf("CustomOutfits: outfit parts: the table %s", on ? "entered" : "left");
    GearPlaySound(state, on ? kSoundSelect : kSoundBack);
    GearRefresh(state);
}

bool IsAction(int action, const int (&list)[3]) { return std::find(std::begin(list), std::end(list), action) != std::end(list); }

void GearMove(void* state, int delta) {
    if (g_gear.navFrame == g_gear.frame) return;   // (the D-pad's second action of the same press)
    g_gear.navFrame = g_gear.frame;
    const int selected = std::clamp(g_gear.selected + delta, 0, GearPartCount() - 1);
    if (selected == g_gear.selected) return;
    g_gear.selected = selected;
    GearPlaySound(state, kSoundNavigate);
    GearRefresh(state);
}

void __fastcall HookGearInputPressed(void* state, int action, std::uintptr_t flag) {
    if (!state || At<std::uint8_t>(state, kGearInputBlocked)) return g_origGearInputPressed(state, action, flag);
    if (g_gear.editing && state == g_gear.state) {
        if (!GearUsable(state)) {
            g_gear.editing = false;
        } else {
            if (IsAction(action, kActionUp)) GearMove(state, -1);
            else if (IsAction(action, kActionDown)) GearMove(state, 1);
            else if (action == kActionSelect) GearToggle(state, g_gear.selected);
            else if (action == kActionClose || action == kActionY) GearSetEditing(state, false);
            return;   // (the list and the menu wait)
        }
    }
    if (action == kActionY && GearUsable(state)) {
        GearSetEditing(state, true);
        return;
    }
    g_origGearInputPressed(state, action, flag);
}

void __fastcall HookGearInputOn(void* state, int action, std::uintptr_t flag) {
    if (g_gear.editing && state && state == g_gear.state && GearUsable(state)) return;   // (held keys: the list waits)
    g_origGearInputOn(state, action, flag);
}

bool GameWindowFocused() {
    HWND window = GetForegroundWindow();
    DWORD process = 0;
    return window && GetWindowThreadProcessId(window, &process) && process == GetCurrentProcessId();
}

bool MouseLeftDown() {
    const int button = GetSystemMetrics(SM_SWAPBUTTON) ? VK_RBUTTON : VK_LBUTTON;
    return (GetAsyncKeyState(button) & 0x8000) != 0;
}

// The mouse in asset_mc's own coordinates (its _xmouse / _ymouse: the panel's place, scale and tilt taken in).
bool PanelMouse(const GfxValue& asset, double& x, double& y) {
    GfxValue mx, my;
    const bool ok = GfxGet(asset, "_xmouse", mx) && GfxGet(asset, "_ymouse", my) && (mx.type & 0x8F) == 3 && (my.type & 0x8F) == 3;
    if (ok) x = mx.number, y = my.number;
    GfxRelease(my);
    GfxRelease(mx);
    return ok;
}
// The table's row under the mouse: -1 none, -2 the mouse's place not read. Each row owns one spacing around its text,
// between the table's left and right lines.
int RowUnderMouse(void* widget, double* atX = nullptr, double* atY = nullptr) {
    if (g_gear.rows <= 0 || g_gear.rowStep <= 0.0) return -1;
    GfxValue asset;
    if (!WidgetClip(widget, "asset_mc", asset, true)) return -1;
    double x = 0.0, y = 0.0;
    const bool read = PanelMouse(asset, x, y);
    GfxRelease(asset);
    if (!read) return -2;
    if (atX) *atX = x;
    if (atY) *atY = y;
    if (x < g_gear.tableLeft || x > g_gear.tableRight) return -1;
    const double k = (y - g_gear.rowCenter0) / g_gear.rowStep + 0.5;
    if (k < 0.0) return -1;
    const int row = static_cast<int>(k);
    return row < g_gear.rows ? row : -1;
}

// The row under the mouse lit (mouse & keyboard): while editing, the mouse chooses the part (as Up / Down).
void GearHover(void* state) {
    void* widget = GearWidget(state);
    int row = -1;
    if (widget && (UsingMouseKeyboard() || !g_gear.mouseKeyboardKnown)) row = std::max(-1, RowUnderMouse(widget));
    const int part = row >= 0 ? g_gear.first + row : -1;
    if (part == g_gear.hover) return;
    g_gear.hover = part;
    if (g_gear.editing && part >= 0 && part != g_gear.selected) {
        g_gear.selected = part;
        GearPlaySound(state, kSoundNavigate);
        GearRefresh(state);   // (the prompt: Show part / Hide part)
        return;
    }
    if (widget) ApplyRowAlpha(widget);
}

// A click on one of the table's rows (its text or its STATE) toggles its part.
void GearClickHitTest(void* state);
void GearClick(void* state) {
    void* widget = GearWidget(state);
    double x = 0.0, y = 0.0;
    const int row = RowUnderMouse(widget, &x, &y);
    if (row == -2) return GearClickHitTest(state);   // (the panel's mouse place not read: the rows' own hitTest)
    if (g_gear.clicksLogged < 3) {
        ++g_gear.clicksLogged;
        Log::Writef("CustomOutfits: outfit parts: click at %.0f, %.0f in the panel (rows from y %.0f, %.0f apart, x %.0f..%.0f): %s",
                    x, y, g_gear.rowCenter0 - g_gear.rowStep / 2.0, g_gear.rowStep, g_gear.tableLeft, g_gear.tableRight,
                    row >= 0 ? "on a row" : "not on the table's rows");
    }
    if (row >= 0) {
        g_gear.selected = g_gear.first + row;
        GearToggle(state, g_gear.selected);
    }
}
void GearClickHitTest(void* state) {
    void* widget = GearWidget(state);
    GfxValue asset, mouseX, mouseY;
    if (!WidgetClip(widget, "asset_mc", asset)) return;
    if (MovieGet(widget, "_root._xmouse", mouseX) && MovieGet(widget, "_root._ymouse", mouseY) &&
        (mouseX.type & 0x8F) == 3 && (mouseY.type & 0x8F) == 3) {
        GfxValue args[3] = {GfxNumber(mouseX.number), GfxNumber(mouseY.number), GfxBool(false)};
        int hit = -1;
        for (int r = 0; r < g_gear.rows && hit < 0; ++r)
            for (const char* prefix : {"resource_", "Wallet_"}) {
                GfxValue row, result;
                if (GearRowClip(asset, prefix, r, row) && GfxCall(row, "hitTest", args, 3, &result) &&
                    (result.type & 0x8F) == 2 && result.boolean)
                    hit = r;
                GfxRelease(result);
                GfxRelease(row);
                if (hit >= 0) break;
            }
        if (g_gear.clicksLogged < 3) {
            ++g_gear.clicksLogged;
            Log::Writef("CustomOutfits: outfit parts: click at %.0f, %.0f (the panel's mouse not read: hitTest): %s", mouseX.number,
                        mouseY.number, hit >= 0 ? "on a row" : "not on the table's rows");
        }
        if (hit >= 0) {
            g_gear.selected = g_gear.first + hit;
            GearToggle(state, g_gear.selected);
        }
    } else if (g_gear.clicksLogged < 3) {
        ++g_gear.clicksLogged;
        Log::Writef("CustomOutfits: outfit parts: click: the mouse position was not read (types %u, %u)", mouseX.type, mouseY.type);
    }
    GfxRelease(mouseY);
    GfxRelease(mouseX);
    GfxRelease(asset);
}

// ---- The Outfits list: only its rows near the view drawn (framework 0.9.405) ----------------------------------------
// The Gear menu's movie (Campsite_GearSelect, the state's clip) attaches every listed outfit to
// campsite_outfit_select_mc (AddOutfitItem: nav_list_item_<n>_mc, five a row, row r at _y m_outfitMcY + 180 * r), and
// scrolls by moving that clip (UpdateOutfitScrollbar: a 0.25 s tween of its _y to m_outfitMcY - 180 * m_viewCursor).
// Two rows show; the others are masked, but Scaleform draws every visible clip, masked or not, every frame (each with
// its picture, frames and text). 613 outfits (580 copies of laracroft) in the list: the game's frames went wrong in
// the campsite (graphics glitches), then the display driver crashed on leaving it. Here, past kListWindowFrom
// outfits, the rows more than kListMarginRows away from the rows in view are hidden (_visible = false) and shown as the
// list scrolls (the clip's _y as it is: during its tween too). The movie shows every item again when it fills the list
// (AddOutfitItem; Reset hides them all): one hidden item is read each frame, and seen shown again, all are set again.
constexpr int kListPerRow = 5;           // NUM_OUTFITS_PER_ROW
constexpr int kListRowsShown = 2;        // NUM_ROW_OUTFITS_VISIBLE
constexpr int kListMarginRows = 1;       // rows kept drawn above and below them
constexpr int kListWindowFrom = 40;      // outfits listed (the game's own 33 stay as the movie makes them)
constexpr double kListRowHeight = 180.0; // m_outfitDeltaY
struct OutfitListWindow {
    void* state = nullptr;
    int items = 0;
    int first = -1, last = -1;   // the rows drawn (-1: none set yet)
};
OutfitListWindow g_listWindow;
bool g_listWindowReported = false;
bool g_listWindowReady = false;   // (GFx functions checked)

bool ListItem(const GfxValue& list, int index, GfxValue& out) {
    char name[40];
    std::snprintf(name, sizeof(name), "nav_list_item_%d_mc", index);
    return GfxClip(list, name, out);
}
void ListItemShow(const GfxValue& list, int index, bool shown) {
    GfxValue item;
    if (!ListItem(list, index, item)) return;
    GfxSet(item, "_visible", GfxBool(shown));
    GfxRelease(item);
}
bool ListItemShown(const GfxValue& list, int index) {
    GfxValue item;
    if (!ListItem(list, index, item)) return false;
    GfxValue visible;
    bool shown = false;
    if (GfxGet(item, "_visible", visible)) {
        shown = (visible.type & 0x8F) == 2 && visible.boolean;
        GfxRelease(visible);
    }
    GfxRelease(item);
    return shown;
}

void UpdateOutfitListWindow(void* state) {
    if (!g_listWindowReady || !state || !GearOnOutfits(state)) return;
    GfxValue clip;   // the state's own value (not released here)
    clip.iface = At<void*>(state, kStateMovieValue);
    clip.type = At<std::uint32_t>(state, kStateMovieValue + 8);
    clip.data = At<void*>(state, kStateMovieValue + 0x10);
    if (!clip.iface || (clip.type & 0x8F) <= 1) return;
    if (GfxGetNumber(clip, "m_internalState", -1.0) != 1.0) return;   // STATE_OUTFIT_SELECT
    const int items = static_cast<int>(GfxGetNumber(clip, "m_navItems", 0.0));
    if (state != g_listWindow.state || items != g_listWindow.items) {
        g_listWindow.state = state;
        g_listWindow.items = items;
        g_listWindow.first = g_listWindow.last = -1;
    }
    if (items <= kListWindowFrom) return;
    GfxValue list;
    if (!GfxClip(clip, "campsite_outfit_select_mc", list)) return;
    const double baseY = GfxGetNumber(clip, "m_outfitMcY", 0.0);
    double rowHeight = GfxGetNumber(clip, "m_outfitDeltaY", kListRowHeight);
    if (!(rowHeight > 1.0)) rowHeight = kListRowHeight;
    const double y = GfxGetNumber(list, "_y", baseY);
    double position = (baseY - y) / rowHeight;   // the first row in view (fractional while it moves)
    if (!std::isfinite(position) || position < 0.0) position = 0.0;
    const int rows = (items + kListPerRow - 1) / kListPerRow;
    const int first = std::max(0, static_cast<int>(std::floor(position)) - kListMarginRows);
    const int last = std::min(rows - 1, static_cast<int>(std::ceil(position)) + kListRowsShown - 1 + kListMarginRows);
    bool all = g_listWindow.first < 0;
    if (!all) {
        // an item outside the rows drawn: shown again means the movie filled the list again
        const int probe = g_listWindow.first > 0 ? 0 : (g_listWindow.last < rows - 1 ? items - 1 : -1);
        if (probe >= 0 && ListItemShown(list, probe)) all = true;
    }
    if (all) {
        for (int i = 0; i < items; ++i) {
            const int row = i / kListPerRow;
            ListItemShow(list, i, row >= first && row <= last);
        }
        if (!g_listWindowReported) {
            g_listWindowReported = true;
            Log::Writef("CustomOutfits: the Outfits list has %d outfits: only the rows near the view are drawn", items);
        }
    } else if (first != g_listWindow.first || last != g_listWindow.last) {
        const int from = std::min(first, g_listWindow.first), to = std::max(last, g_listWindow.last);
        for (int row = from; row <= to; ++row) {
            const bool was = row >= g_listWindow.first && row <= g_listWindow.last;
            const bool now = row >= first && row <= last;
            if (was == now) continue;
            for (int i = row * kListPerRow; i < std::min(items, (row + 1) * kListPerRow); ++i) ListItemShow(list, i, now);
        }
    }
    g_listWindow.first = first;
    g_listWindow.last = last;
    GfxRelease(list);
}

bool g_gearPartsOn = false;   // the parts table's hooks' work (InstallGearParts)

std::uintptr_t __fastcall HookGearProcess(void* state, float dt, void* c, void* d) {
    const std::uintptr_t result = g_origGearProcess(state, dt, c, d);
    UpdateOutfitListWindow(state);
    if (!g_gearPartsOn) return result;
    ++g_gear.frame;
    const bool focused = GameWindowFocused();
    const bool down = focused && MouseLeftDown();
    const bool key = focused && (GetAsyncKeyState(g_gear.key) & 0x8000) != 0;
    // in the table: Enter and E toggle the chosen part (the keyboard's Enter is not UI_Select here)
    const bool toggleKey = focused && ((GetAsyncKeyState(VK_RETURN) & 0x8000) != 0 || (GetAsyncKeyState('E') & 0x8000) != 0);
    const bool clicked = down && !g_gear.mouseDown, typed = key && !g_gear.keyDown;
    const bool toggled = toggleKey && !g_gear.toggleKeyDown;
    g_gear.mouseDown = down;
    g_gear.keyDown = key;
    g_gear.toggleKeyDown = toggleKey;
    if (!state || At<std::uint8_t>(state, kGearInputBlocked) || !GearUsable(state)) {
        g_gear.hover = -1;
        return result;
    }
    GearHover(state);
    if (typed) GearSetEditing(state, !g_gear.editing);
    else if (toggled && g_gear.editing) GearToggle(state, g_gear.selected);
    else if (clicked) GearClick(state);
    return result;
}

std::uintptr_t __fastcall HookGearUpdateInformation(void* state, void* a, void* b, void* c) {
    const int config = state ? GearConfig(state) : -1;
    const int highlight = state ? At<std::int32_t>(state, kStateSelectedIndex) : -1;
    if (state != g_gear.state || highlight != g_gear.highlight || config != g_gear.config) {
        g_gear.editing = false;
        g_gear.selected = 0;
        g_gear.first = 0;
        g_gear.hover = -1;
    }
    g_gear.state = state;
    g_gear.highlight = highlight;
    g_gear.config = config;
    const std::uintptr_t result = g_origGearUpdateInformation(state, a, b, c);
    if (state && GearOnOutfits(state))
        if (void* widget = GearWidget(state)) {
            GearDrawRows(state, widget, false);
            HideTransitionTable(widget);
        }
    return result;
}

// The list's own select (the keyboard's Enter, a click on an outfit): nothing while in the table.
std::uintptr_t __fastcall HookGearOnUiSelect(void* state, void* a, void* b, void* c) {
    if (g_gear.editing && state && state == g_gear.state && GearUsable(state)) return 0;
    return g_origGearOnUiSelect(state, a, b, c);
}

// UiMenuStateCampsiteGear::EquipSelectedWeapon(signed char) (the Outfits list's equip too: the highlighted row's
// outfit; it equips it only when a character profile wears its object, then the respawn looks the profile up again):
// the outfit's object gets a profile slot first (GiveSlot). Kept from it: the objects worn, being changed from and to
// (+0x264 m_currentWornOutfit, +0x268 m_previousWornOutfit: object IDs; +0x270 the outfit being loaded), and the one
// remembered as worn last.
constexpr std::uintptr_t kRvaEquipSelected = 0x008E8F80ull;
constexpr std::uint8_t kEquipSelectedSig[] = {0x40, 0x53, 0x55, 0x56, 0x57, 0x41, 0x56, 0x48, 0x83,
                                              0xEC, 0x40, 0x48, 0x63, 0x81, 0xF4, 0x01, 0x00, 0x00};
constexpr std::ptrdiff_t kGearWornObject = 0x264;
constexpr std::ptrdiff_t kGearPreviousObject = 0x268;
constexpr std::ptrdiff_t kGearLoadingOutfit = 0x270;
using EquipSelectedFn = std::uintptr_t(__fastcall*)(void*, void*, void*, void*);
EquipSelectedFn g_origEquipSelected = nullptr;
std::uintptr_t __fastcall HookEquipSelected(void* state, void* a, void* b, void* c) {
    if (state && GearOnOutfits(state)) {
        const int row = At<std::int32_t>(state, kStateSelectedIndex);
        const std::uint8_t* entries = At<std::uint8_t*>(state, kGearEntries);
        const std::uint8_t* outfit = nullptr;
        // (the outfits' rows are their sorted order: row n is entry n; the broken parts' rows come after)
        if (entries && row >= 0 && row < At<std::int32_t>(state, kGearCount) &&
            CopyGuarded(&outfit, entries + std::size_t(row) * 32, sizeof(outfit)) && outfit) {
            std::vector<std::uint16_t> protect{Features::CurrentOutfitObjectId(),
                                               static_cast<std::uint16_t>(At<std::uint32_t>(state, kGearWornObject)),
                                               static_cast<std::uint16_t>(At<std::uint32_t>(state, kGearPreviousObject))};
            if (const std::uint8_t* loading = At<const std::uint8_t*>(state, kGearLoadingOutfit))
                protect.push_back(ReadU16(loading, kOutfitPlayerObject));
            const int last = g_lastWornConfig.load();
            if (last >= 0 && static_cast<std::size_t>(last) < g_configs.size())
                protect.push_back(g_configs[static_cast<std::size_t>(last)].object);
            GiveSlot(ReadU16(outfit, kOutfitPlayerObject), protect);
        }
    }
    return g_origEquipSelected(state, a, b, c);
}

std::uintptr_t __fastcall HookGearOnExit(void* state, void* a, void* b, void* c) {
    if (state == g_listWindow.state) g_listWindow = OutfitListWindow{};
    if (state == g_gear.state) {
        g_gear.state = nullptr;
        g_gear.editing = false;
        g_gear.config = -1;
        g_gear.highlight = -1;
        g_gear.rows = 0;
        g_gear.headerMode = -1;   // (the panel may be loaded again: its titles set again)
    }
    g_menuConfig.store(-1);
    return g_origGearOnExit(state, a, b, c);
}

const char* __fastcall HookGetLocalString(void* manager, const char* key) {
    if (key) {
        const char* bare = key[0] == '$' ? key + 1 : key;
        for (const char* label : {kLabelEdit, kLabelShow, kLabelHide, kLabelDone, g_gear.editKeyLabel.c_str()})
            if (std::strcmp(bare, label) == 0) return label;
    }
    return g_origGetLocalString(manager, key);
}

// The Gear menu's Process and OnExit hooks: the Outfits list's rows near the view (UpdateOutfitListWindow), and the
// parts table's work when InstallGearParts put it in place.
void InstallGearMenu() {
    struct Called {
        std::uintptr_t rva;
        const std::uint8_t* sig;
        std::size_t size;
        const char* name;
    };
    const Called called[] = {
        {kRvaGfxGetMember, kGfxGetMemberSig, sizeof(kGfxGetMemberSig), "GFxValue::ObjectInterface::GetMember"},
        {kRvaGfxSetMember, kGfxSetMemberSig, sizeof(kGfxSetMemberSig), "GFxValue::ObjectInterface::SetMember"},
        {kRvaGfxObjectRelease, kGfxObjectReleaseSig, sizeof(kGfxObjectReleaseSig), "GFxValue::ObjectInterface::ObjectRelease"},
    };
    for (const auto& c : called)
        if (!GameBuild::VerifyBytes(c.rva, c.sig, c.size)) {
            Log::Writef("CustomOutfits: %s signature mismatch; the Outfits list draws all its rows", c.name);
            return;
        }
    const std::int32_t* types = Game<const std::int32_t*>(kRvaGearCategoryTypes);
    if (std::count(types, types + kGearCategoryCount, kOutfitsCategoryType) != 1) {
        Log::Write("CustomOutfits: the Gear menu's categories were not found; the Outfits list draws all its rows");
        return;
    }
    if (!CreateHook(kRvaGearProcess, kGearProcessSig, sizeof(kGearProcessSig), reinterpret_cast<void*>(&HookGearProcess),
                    reinterpret_cast<void**>(&g_origGearProcess), "CampsiteGear::Process"))
        return;
    if (!CreateHook(kRvaGearOnExit, kGearOnExitSig, sizeof(kGearOnExitSig), reinterpret_cast<void*>(&HookGearOnExit),
                    reinterpret_cast<void**>(&g_origGearOnExit), "CampsiteGear::OnExit")) {
        MH_RemoveHook(reinterpret_cast<void*>(GameBuild::Base() + kRvaGearProcess));
        g_origGearProcess = nullptr;
        return;
    }
    g_listWindowReady = true;
}

void InstallGearParts() {
    char exe[MAX_PATH]{};
    GetModuleFileNameA(nullptr, exe, MAX_PATH);
    const std::string ini = (fs::path(exe).parent_path() / "ROTTRFramework.ini").string();
    if (!GetPrivateProfileIntA("Outfits", "OutfitsScreenParts", 1, ini.c_str())) {
        Log::Detail("CustomOutfits: outfit parts: off ([Outfits] OutfitsScreenParts=0)");
        return;
    }
    // the keyboard's key for the table (Y on a game pad): a letter or digit, default T
    char key[16]{};
    GetPrivateProfileStringA("Outfits", "OutfitsScreenPartsKey", "T", key, sizeof(key), ini.c_str());
    const char k = static_cast<char>(std::toupper(static_cast<unsigned char>(key[0])));
    if ((k >= 'A' && k <= 'Z') || (k >= '0' && k <= '9')) g_gear.key = k;
    g_gear.editKeyLabel = std::string("Edit parts [") + static_cast<char>(g_gear.key) + "]";
    g_gear.mouseKeyboardKnown = GameBuild::VerifyBytes(kRvaIsMouseKeyboardStatus, kIsMouseKeyboardStatusSig,
                                                      sizeof(kIsMouseKeyboardStatusSig));
    if (!g_gear.mouseKeyboardKnown)
        Log::Write("CustomOutfits: outfit parts: the mouse & keyboard flag was not found (the prompt names no key)");
    struct Called {
        std::uintptr_t rva;
        const std::uint8_t* sig;
        std::size_t size;
        const char* name;
    };
    const Called called[] = {
        {kRvaWidgetManagerGet, kWidgetManagerGetSig, sizeof(kWidgetManagerGetSig), "UiWidgetManager::Get"},
        {kRvaGetWidget, kGetWidgetSig, sizeof(kGetWidgetSig), "UiWidgetManagerBase::GetWidget"},
        {kRvaInvokeASFunction, kInvokeASFunctionSig, sizeof(kInvokeASFunctionSig), "UiWidgetScaleform::InvokeASFunction"},
        {kRvaWidgetGetVariable, kWidgetGetVariableSig, sizeof(kWidgetGetVariableSig), "UiWidgetScaleform::GetVariable"},
        {kRvaGfxGetMember, kGfxGetMemberSig, sizeof(kGfxGetMemberSig), "GFxValue::ObjectInterface::GetMember"},
        {kRvaGfxSetMember, kGfxSetMemberSig, sizeof(kGfxSetMemberSig), "GFxValue::ObjectInterface::SetMember"},
        {kRvaGfxInvoke, kGfxInvokeSig, sizeof(kGfxInvokeSig), "GFxValue::ObjectInterface::Invoke"},
        {kRvaGfxObjectRelease, kGfxObjectReleaseSig, sizeof(kGfxObjectReleaseSig), "GFxValue::ObjectInterface::ObjectRelease"},
        {kRvaGetContextbar, kGetContextbarSig, sizeof(kGetContextbarSig), "UiMenuStateGeneric::GetContextbarWidget"},
        {kRvaClearItems, kClearItemsSig, sizeof(kClearItemsSig), "UiWidgetContextbar::ClearItems"},
        {kRvaAddItem, kAddItemSig, sizeof(kAddItemSig), "UiWidgetContextbar::AddItem"},
    };
    for (const auto& c : called)
        if (!GameBuild::VerifyBytes(c.rva, c.sig, c.size)) {
            Log::Writef("CustomOutfits: outfit parts: %s signature mismatch; the Outfits screen is left as it is", c.name);
            return;
        }
    // the category types: Outfits among them
    const std::int32_t* types = Game<const std::int32_t*>(kRvaGearCategoryTypes);
    if (std::count(types, types + kGearCategoryCount, kOutfitsCategoryType) != 1) {
        Log::Write("CustomOutfits: outfit parts: the Gear menu's categories were not found; the Outfits screen is left as it is");
        return;
    }
    struct Hook {
        std::uintptr_t rva;
        const std::uint8_t* sig;
        std::size_t size;
        void* detour;
        void** original;
        const char* name;
    };
    const Hook hooks[] = {
        {kRvaGearUpdateInformation, kGearUpdateInformationSig, sizeof(kGearUpdateInformationSig),
         reinterpret_cast<void*>(&HookGearUpdateInformation), reinterpret_cast<void**>(&g_origGearUpdateInformation),
         "CampsiteGear::UpdateInformationWidgetAndButtonLegend"},
        {kRvaGearButtonLegend, kGearButtonLegendSig, sizeof(kGearButtonLegendSig), reinterpret_cast<void*>(&HookGearButtonLegend),
         reinterpret_cast<void**>(&g_origGearButtonLegend), "CampsiteGear::UpdateButtonLegend"},
        {kRvaGearInputPressed, kGearInputPressedSig, sizeof(kGearInputPressedSig), reinterpret_cast<void*>(&HookGearInputPressed),
         reinterpret_cast<void**>(&g_origGearInputPressed), "CampsiteGear::OnInputActionPressed"},
        {kRvaGearInputOn, kGearInputOnSig, sizeof(kGearInputOnSig), reinterpret_cast<void*>(&HookGearInputOn),
         reinterpret_cast<void**>(&g_origGearInputOn), "CampsiteGear::OnInputActionOn"},
        {kRvaGearOnUiSelect, kGearOnUiSelectSig, sizeof(kGearOnUiSelectSig), reinterpret_cast<void*>(&HookGearOnUiSelect),
         reinterpret_cast<void**>(&g_origGearOnUiSelect), "CampsiteGear::OnUiSelect"},
        {kRvaGetLocalString, kGetLocalStringSig, sizeof(kGetLocalStringSig), reinterpret_cast<void*>(&HookGetLocalString),
         reinterpret_cast<void**>(&g_origGetLocalString), "MenuManager::GetLocalString"},
    };
    for (const auto& h : hooks)
        if (!GameBuild::VerifyBytes(h.rva, h.sig, h.size)) {
            Log::Writef("CustomOutfits: outfit parts: %s signature mismatch; the Outfits screen is left as it is", h.name);
            return;
        }
    int created = 0;
    for (const auto& h : hooks) {
        if (!CreateHook(h.rva, h.sig, h.size, h.detour, h.original, h.name)) break;
        ++created;
    }
    if (created == static_cast<int>(sizeof(hooks) / sizeof(hooks[0]))) {
        if (!g_origGearProcess || !g_origGearOnExit) {   // (InstallGearMenu's: the table needs them)
            for (int i = 0; i < created; ++i) MH_RemoveHook(reinterpret_cast<void*>(GameBuild::Base() + hooks[i].rva));
            Log::Write("CustomOutfits: outfit parts: the Gear menu's Process / OnExit hooks are missing; the Outfits screen is left as it is");
            return;
        }
        g_gearPartsOn = true;
        Log::Detailf("CustomOutfits: outfit parts: the Outfits screen lists an outfit's toggles (Y / %c: edit them)", g_gear.key);
        return;
    }
    for (int i = 0; i < created; ++i) MH_RemoveHook(reinterpret_cast<void*>(GameBuild::Base() + hooks[i].rva));
    Log::Write("CustomOutfits: outfit parts: hooks could not be created; the Outfits screen is left as it is");
}

bool g_configsLoaded = false;

void EnsureConfigsLoaded() {
    if (g_configsLoaded) return;
    g_configsLoaded = true;
    LoadConfigs();
}
}  // namespace

void StartEarly() {
    if (!GameBuild::IsSupportedRetail() || g_buildStarted.exchange(true)) return;
    EnsureConfigsLoaded();
    std::vector<OutfitArchive::OutfitSpec> specs;
    for (const auto& config : g_configs)
        if (config.buildsModel) {
            specs.push_back({config.key, config.objectName, config.model.empty() ? config.baseName : config.model,
                             config.model.empty() && !config.cloneOnly, config.folder, config.sources,
                             config.cloneOnly, config.exclude, config.picture});
            specs.back().shared = config.shared;
            // a retail outfit replaced with a model built into its own object (mod.json "object" = "base")
            specs.back().replacesObject = !config.replaces.empty() && !config.cloneOnly && config.objectName == config.baseName;
            // (the Outfit Manager looks for this text in the .asi to tell a framework that builds retail objects)
            if (specs.back().replacesObject)
                Log::Detailf("CustomOutfits: %s: its model replaces the game's object %s", config.key.c_str(),
                            config.objectName.c_str());
        }
    // game models replaced (mod.json "models"): built into the game's own object, as a retail outfit's replacement
    for (const auto& model : g_modelConfigs) {
        // (an outfit already building that object wins)
        if (std::any_of(specs.begin(), specs.end(), [&](const OutfitArchive::OutfitSpec& s) { return s.objectName == model.objectName; })) {
            Log::Writef("CustomOutfits: model %s skipped: an outfit already builds %s", model.key.c_str(), model.objectName.c_str());
            continue;
        }
        OutfitArchive::OutfitSpec spec;
        spec.key = model.key;
        spec.objectName = model.objectName;
        spec.baseObjectName = model.objectName;
        spec.folder = model.folder;
        spec.exclude = model.exclude;
        spec.shared = model.shared;
        spec.replacesObject = true;
        specs.push_back(std::move(spec));
        // (the Modding Suite looks for this text in the .asi to tell a framework that replaces game models)
        Log::Detailf("CustomOutfits: %s: replaces the game object's model %s (mod.json models)", model.key.c_str(),
                    model.objectName.c_str());
    }
    WeaponMods::AddSpecs(specs);   // (v0.9.565) mods' new weapons: copies of game weapons under new objects
    std::error_code error;
    const bool staleArchive = fs::exists(GameRoot() / OutfitArchive::NfoFileName(), error);
    if (!g_streams.empty())
        // (the Modding Suite looks for this text in the .asi to tell a framework that puts mods' streams in its archive)
        Log::Detailf("CustomOutfits: %zu sound stream(s) of mods go into the framework archive", g_streams.size());
    if (specs.empty() && g_streams.empty() && !staleArchive) return;

    // Hold the game's archive scan until the archive is written (the scan normally comes after this point).
    g_buildDoneEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    g_buildDone.store(false, std::memory_order_release);
    void* target = reinterpret_cast<void*>(GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "FindFirstFileExA"));
    if (target && MH_CreateHook(target, reinterpret_cast<void*>(&HookFindFirstFileExA),
                                reinterpret_cast<void**>(&g_origFindFirstFileExA)) == MH_OK &&
        MH_EnableHook(target) == MH_OK)
        Log::Detail("CustomOutfits: the game's archive scan waits for the custom outfit archive");
    else
        Log::Write("CustomOutfits: archive scan hook unavailable; new outfits may need a second start the first time");
    Log::Detailf("CustomOutfits: building the custom outfit archive for %zu outfit(s) with new models", specs.size());
    try {
        std::thread(BuildArchiveThread, std::move(specs), g_streams).detach();
    } catch (...) {
        g_buildDone.store(true);
        SetEvent(g_buildDoneEvent);
        Log::Write("CustomOutfits: could not start the archive build");
    }
}

// ---- hosted animation graphs ----------------------------------------------------------------------------------------
// Weapons (and other hosts of animations) give the player graphs for aiming etc. through
// cdc::AnimGraphManager::GetHostedGraphParamsByUserAndType(manager, userInstance, type, index, flag): the hosted
// graph's manager lists the user objects it supports (manager+0x50 -> +0x70 entries, +0x78 count; 0x28 per entry:
// +0 u16 user zone ID, +2 s8 graph type, +0x10 index (type 0x15), +0x1C byte: also match the user's base zones), and
// the user Instance's zone (Instance+0x80 -> ObjectZone+8 -> ObjectZoneSharedData+0x50) has to be listed. A new
// object is in no list, so Lara stayed in the bind pose while aiming. When the game finds nothing for a new object,
// the lookup is repeated as its base object (laracroft).
constexpr std::uintptr_t kRvaHostedGraphParams = 0x001141A0ull;
constexpr std::uint8_t kHostedGraphParamsSig[] = {0x48, 0x89, 0x74, 0x24, 0x20, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57,
                                                  0x45, 0x33, 0xD2, 0x45, 0x33, 0xF6, 0x45, 0x8B, 0xF9, 0x41, 0x8B,
                                                  0xF0, 0x4C, 0x8B, 0xEA, 0x48, 0x85, 0xD2, 0x75, 0x0E};
using HostedGraphParamsFn = void*(__fastcall*)(void*, void*, int, unsigned int, bool);
HostedGraphParamsFn g_origHostedGraphParams = nullptr;
// Each new object's base object, by zone ID (zone IDs are 16 bits): 0.9.403 and before kept 32 pairs, so from the
// 33rd new object on (counting the outfits in their mods' order) the lookups below found nothing: no hosted graphs
// (Lara in the bind pose in those outfits) and no grunts. Read lock-free (game and audio threads).
std::atomic<std::uint16_t> g_baseZones[0x10000];
std::atomic<int> g_zoneAliasCount{0};
std::vector<std::uint16_t> g_aliasedZones;   // (game thread) the zones set in g_baseZones
std::uint64_t g_aliasKey = 0;                // (game thread) the pairs set, hashed

std::uint16_t BaseZoneOf(std::uint32_t zone) {
    return zone < 0x10000 ? g_baseZones[zone].load(std::memory_order_relaxed) : 0;
}

void* FindHostedGraphParams(std::uint8_t* manager, int type, unsigned int index, std::uint16_t zone) noexcept {
    __try {
        auto* owner = *reinterpret_cast<std::uint8_t**>(manager + 0x50);
        if (!owner || *reinterpret_cast<int*>(owner + 0x38) < 1) return nullptr;
        const int count = *reinterpret_cast<int*>(owner + 0x78);
        auto* entries = *reinterpret_cast<std::uint8_t**>(owner + 0x70);
        for (int i = 0; i < count && entries; ++i) {
            std::uint8_t* entry = entries + std::size_t(i) * 0x28;
            if (static_cast<int>(static_cast<std::int8_t>(entry[2])) != type) continue;
            if (type == 0x15 && *reinterpret_cast<unsigned int*>(entry + 0x10) != index) continue;
            if (*reinterpret_cast<std::uint16_t*>(entry) == zone) return entry;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    return nullptr;
}

std::uint32_t InstanceZone(void* instance) noexcept {
    __try {
        auto* zoneRef = *reinterpret_cast<std::uint8_t**>(static_cast<std::uint8_t*>(instance) + 0x80);
        if (!zoneRef) return 0;
        auto* shared = *reinterpret_cast<std::uint8_t**>(zoneRef + 8);
        return shared ? *reinterpret_cast<std::uint32_t*>(shared + 0x50) : 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

void* __fastcall HookHostedGraphParams(void* manager, void* user, int type, unsigned int index, bool flag) {
    void* found = g_origHostedGraphParams(manager, user, type, index, flag);
    if (found || !manager || !user || g_zoneAliasCount.load(std::memory_order_acquire) == 0) return found;
    const std::uint16_t base = BaseZoneOf(InstanceZone(user));
    return base ? FindHostedGraphParams(static_cast<std::uint8_t*>(manager), type, index, base) : nullptr;
}

// ---- sound plex selectors (Lara's grunts) ---------------------------------------------------------------------------
// The voice banks pick a plex by the speaker through cdc::SoundPlexSelector: its branch picker (0x39C3B0, data, value,
// selector) looks for a branch keyed with the value (data+8: 0x18 each, +0x10 -> u32 key; count at data+0x10). For a
// zone selector (type 2, data+0x15) the value is the speaker's object zone, and a branch marked "also its bases" takes
// the objects listing that zone as a base; a new object lists none of them, so it matched no branch and the selector
// gave nothing: Lara's grunts were silent in the extra outfit slots (the grapple throw's sound has no selector). When
// the game finds nothing for a new object, the pick is repeated as its base object. (The audio thread: lock-free.)
constexpr std::uintptr_t kRvaSoundPlexSelectorPick = 0x0039C3B0ull;
constexpr std::uint8_t kSoundPlexSelectorPickSig[] = {0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x6C, 0x24, 0x10, 0x48,
                                                      0x89, 0x74, 0x24, 0x18, 0x57, 0x41, 0x56, 0x41, 0x57, 0x48, 0x83,
                                                      0xEC, 0x20, 0x44, 0x8B, 0x71, 0x10, 0x45, 0x33, 0xFF};
constexpr std::uint8_t kSoundSelectorZoneType = 2;
using SoundPlexSelectorPickFn = void*(__fastcall*)(void*, std::uint32_t, void*);
SoundPlexSelectorPickFn g_origSoundPlexSelectorPick = nullptr;

void* __fastcall HookSoundPlexSelectorPick(void* data, std::uint32_t value, void* selector) {
    void* found = g_origSoundPlexSelectorPick(data, value, selector);
    if (found || !data || value == 0 || g_zoneAliasCount.load(std::memory_order_acquire) == 0) return found;
    if (static_cast<const std::uint8_t*>(data)[0x15] != kSoundSelectorZoneType) return found;
    const std::uint16_t base = BaseZoneOf(value);
    return base && base != value ? g_origSoundPlexSelectorPick(data, base, selector) : found;
}

void UpdateZoneAliases() {
    // (every tick: only a change of the pairs rewrites the table)
    std::vector<std::pair<std::uint16_t, std::uint16_t>> pairs = WeaponMods::Aliases();   // (v0.9.565) new weapons too
    for (const auto& config : g_configs)
        if (config.object && config.baseObject && config.object != config.baseObject) pairs.push_back({config.object, config.baseObject});
    std::uint64_t key = 0xCBF29CE484222325ull;
    const int count = static_cast<int>(pairs.size());
    for (const auto& [object, base] : pairs) key = (key ^ ((std::uint64_t(object) << 16) | base)) * 0x100000001B3ull;
    if (key == g_aliasKey && count == g_zoneAliasCount.load(std::memory_order_relaxed)) return;
    g_aliasKey = key;
    std::vector<std::uint16_t> zones;
    zones.reserve(static_cast<std::size_t>(count));
    for (const auto& [object, base] : pairs) {
        g_baseZones[object].store(base, std::memory_order_relaxed);
        zones.push_back(object);
    }
    for (const std::uint16_t zone : g_aliasedZones)
        if (std::find(zones.begin(), zones.end(), zone) == zones.end()) g_baseZones[zone].store(0, std::memory_order_relaxed);
    g_aliasedZones = std::move(zones);
    g_zoneAliasCount.store(count, std::memory_order_release);
    Log::Writef("CustomOutfits: %d new object(s) stand in for their base object in animation graph and sound lookups", count);
}

// ---- level player lists ---------------------------------------------------------------------------------------------
// Every level lists the player objects it supports (dtp::LevelData, reached through Level+0x30 -> UnitData,
// UnitData+0x8C8 -> GameUnitData, GameUnitData+8 -> LevelData):
//   +0x00 player_info* m_pPlayer (1 byte each: the character profile index), +0x08 m_playerListCount,
//   +0x10 player_info_TR* m_pPlayer_TR (0x18 each: u16 objectID, int teamID, inventory*, playerState*),
//   +0x18 m_playerListCount_TR, ..., +0x4C player_info player_list[] (inline).
// Player::InitFromLevelData and InventoryComponent::InitFromLevelData look the player's object up there by its zone
// ID (ObjectZoneSharedData+0x50) to get its player state (moves, abilities) and its inventory. A new object is in no
// level's list, so Lara got neither: stuck in the bind pose, unable to move, and the campsite's gear menu never
// opened. The fix: each loaded level gets a copy of its LevelData with an entry for every new object, copied from
// its base object's entry (the original data stays untouched; the copy is pointed to from GameUnitData).
// Loaded levels: the stream tracker array that STREAM_GetLevelWithID walks (0x2A0 per tracker: +0 level ID,
// +4 state (& 0x7F == 2: loaded), +0x10 Level*).
constexpr std::uintptr_t kRvaGetLevelWithId = 0x004215F0ull;   // STREAM_GetLevelWithID(int)
constexpr std::uint8_t kGetLevelWithIdSig[] = {0x4C, 0x8D, 0x15, 0xD9, 0xCC, 0x91, 0x02, 0x45, 0x33, 0xC0, 0x49, 0x8B,
                                               0xC2, 0x4C, 0x8D, 0x0D, 0xCC, 0x20, 0x92, 0x02, 0x0F, 0xB6, 0x50, 0x04,
                                               0x80, 0xE2, 0x7F, 0x80, 0xFA, 0x02};
constexpr std::size_t kTrackerStride = 0x2A0;
constexpr std::ptrdiff_t kLevelUnitData = 0x30;
constexpr std::ptrdiff_t kUnitGameUnitData = 0x8C8;
constexpr std::ptrdiff_t kGameUnitLevelData = 0x08;
constexpr std::size_t kLevelDataHeader = 0x4C;
constexpr std::size_t kPlayerTrStride = 0x18;
std::uintptr_t g_trackersBegin = 0;
std::uintptr_t g_trackersEnd = 0;
bool g_levelPatchReady = false;
std::mutex g_levelMutex;
std::vector<std::unique_ptr<std::uint8_t[]>> g_levelBuffers;   // our LevelData copies (kept: the game may hold them)
std::vector<std::pair<const std::uint8_t*, std::uint8_t*>> g_levelCopies;   // original -> our copy
std::vector<const std::uint8_t*> g_levelsChecked;   // originals that need no entry (or could not be patched)

bool InitLevelPatch() {
    if (!GameBuild::VerifyBytes(kRvaGetLevelWithId, kGetLevelWithIdSig, sizeof(kGetLevelWithIdSig))) {
        Log::Write("CustomOutfits: STREAM_GetLevelWithID signature mismatch; new objects get no level player data");
        return false;
    }
    const auto base = GameBuild::Base();
    std::int32_t first = 0, last = 0;
    std::memcpy(&first, reinterpret_cast<const void*>(base + kRvaGetLevelWithId + 3), 4);
    std::memcpy(&last, reinterpret_cast<const void*>(base + kRvaGetLevelWithId + 16), 4);
    g_trackersBegin = base + kRvaGetLevelWithId + 7 + first;
    g_trackersEnd = base + kRvaGetLevelWithId + 20 + last;
    return g_trackersEnd > g_trackersBegin && (g_trackersEnd - g_trackersBegin) % kTrackerStride == 0;
}

// One level: adds the missing new objects to its player list. Game thread.
void PatchLevelData(std::uint8_t* gameUnit) {
    std::uint8_t* levelData = nullptr;
    if (!CopyGuarded(&levelData, gameUnit + kGameUnitLevelData, sizeof(levelData)) || !levelData) return;
    for (const auto& [original, copy] : g_levelCopies)
        if (copy == levelData) return;   // ours already
    for (const auto* checked : g_levelsChecked)
        if (checked == levelData) return;
    for (const auto& [original, copy] : g_levelCopies)
        if (original == levelData) {   // the game put the original back: point at our copy again
            std::memcpy(gameUnit + kGameUnitLevelData, &copy, sizeof(copy));
            return;
        }
    std::uint8_t header[kLevelDataHeader]{};
    if (!CopyGuarded(header, levelData, sizeof(header))) return;
    std::uint8_t* players = nullptr;
    std::uint8_t* playersTr = nullptr;
    std::uint32_t count = 0, countTr = 0;
    std::memcpy(&players, header + 0x00, 8);
    std::memcpy(&count, header + 0x08, 4);
    std::memcpy(&playersTr, header + 0x10, 8);
    std::memcpy(&countTr, header + 0x18, 4);
    g_levelsChecked.push_back(levelData);   // replaced by a copy below when entries are added
    if (count == 0 || count > 64 || countTr != count || !players || !playersTr) {
        Log::Detailf("CustomOutfits: level player list not patched (players %u at %p, TR %u at %p)", count,
                    static_cast<void*>(players), countTr, static_cast<void*>(playersTr));
        return;
    }
    std::vector<std::uint8_t> profiles(count);
    std::vector<std::uint8_t> entries(std::size_t(count) * kPlayerTrStride);
    if (!CopyGuarded(profiles.data(), players, profiles.size()) ||
        !CopyGuarded(entries.data(), playersTr, entries.size()))
        return;
    std::string listed;
    for (std::uint32_t i = 0; i < count; ++i) {
        if (i) listed += ',';
        listed += std::to_string(ReadU16(entries.data() + std::size_t(i) * kPlayerTrStride, 0));
    }
    std::string added;
    for (const auto& config : g_configs) {
        // (an outfit that cannot have a character profile is not in the campsite: never worn)
        if (!config.object || !config.baseObject || config.object == config.baseObject || config.profileBase < 0) continue;
        int baseEntry = -1;
        bool present = false;
        for (std::size_t i = 0; i < profiles.size(); ++i) {
            const std::uint16_t object = ReadU16(entries.data() + i * kPlayerTrStride, 0);
            if (object == config.object) present = true;
            if (object == config.baseObject && baseEntry < 0) baseEntry = static_cast<int>(i);
        }
        if (present || baseEntry < 0) continue;
        const std::size_t at = entries.size();
        entries.resize(at + kPlayerTrStride);
        std::memcpy(entries.data() + at, entries.data() + std::size_t(baseEntry) * kPlayerTrStride, kPlayerTrStride);
        WriteU16(entries.data() + at, 0, config.object);
        // (the list's profiles are only the level's choice when the player has none; a slot changes hands: the base's)
        profiles.push_back(profiles[static_cast<std::size_t>(baseEntry)]);
        if (!added.empty()) added += ", ";
        added += config.objectName + " (" + std::to_string(config.object) + ", as " +
                 std::to_string(config.baseObject) + ")";
    }
    if (added.empty()) {
        return;
    }
    // The copy: the header, the inline profile list (extended), then the extended TR list.
    const std::size_t newCount = profiles.size();
    const std::size_t trAt = (kLevelDataHeader + newCount + 15) & ~std::size_t(15);
    auto buffer = std::make_unique<std::uint8_t[]>(trAt + entries.size());
    std::uint8_t* copy = buffer.get();
    std::memcpy(copy, header, kLevelDataHeader);
    std::memcpy(copy + kLevelDataHeader, profiles.data(), newCount);
    std::memcpy(copy + trAt, entries.data(), entries.size());
    std::uint8_t* newPlayers = copy + kLevelDataHeader;
    std::uint8_t* newTr = copy + trAt;
    const std::uint32_t count32 = static_cast<std::uint32_t>(newCount);
    std::memcpy(copy + 0x00, &newPlayers, 8);
    std::memcpy(copy + 0x08, &count32, 4);
    std::memcpy(copy + 0x10, &newTr, 8);
    std::memcpy(copy + 0x18, &count32, 4);
    g_levelsChecked.pop_back();
    g_levelCopies.push_back({levelData, copy});
    g_levelBuffers.push_back(std::move(buffer));
    std::memcpy(gameUnit + kGameUnitLevelData, &copy, sizeof(copy));
    Log::Detailf("CustomOutfits: level player list (objects %s): added %s", listed.c_str(), added.c_str());
}

// Whether new objects are known (their zone IDs and profiles: ApplyProfiles has run): until then nothing is added.
// (0.9.400 and before: nothing at all while any outfit had no profile, so past the profiles' room every new object
// spawned without its level data.)
bool LevelPatchWanted() {
    if (!g_levelPatchReady || !g_profilesApplied) return false;
    for (const auto& config : g_configs)
        if (config.object && config.baseObject && config.object != config.baseObject && config.profileBase >= 0) return true;
    return false;
}

void PatchLevel(std::uint8_t* level) {
    std::uint8_t* unit = nullptr;
    std::uint8_t* gameUnit = nullptr;
    if (!level || !CopyGuarded(&unit, level + kLevelUnitData, sizeof(unit)) || !unit) return;
    if (!CopyGuarded(&gameUnit, unit + kUnitGameUnitData, sizeof(gameUnit)) || !gameUnit) return;
    PatchLevelData(gameUnit);
}

void PatchLoadedLevels() {
    if (!LevelPatchWanted()) return;
    std::lock_guard<std::mutex> lock(g_levelMutex);
    for (std::uintptr_t tracker = g_trackersBegin; tracker + kTrackerStride <= g_trackersEnd; tracker += kTrackerStride) {
        std::uint8_t state = 0;
        std::uint8_t* level = nullptr;
        if (!CopyGuarded(&state, reinterpret_cast<const void*>(tracker + 4), 1) || (state & 0x7F) != 2) continue;
        if (!CopyGuarded(&level, reinterpret_cast<const void*>(tracker + 0x10), sizeof(level))) continue;
        PatchLevel(level);
    }
}

using GetLevelWithIdFn = void*(__fastcall*)(int);
GetLevelWithIdFn g_origGetLevelWithId = nullptr;
// Player::InitFromLevelData looks its level up right before reading the player list: patch that level first.
void* __fastcall HookGetLevelWithId(int id) {
    void* level = g_origGetLevelWithId(id);
    if (level && GetCurrentThreadId() == g_gameThreadId.load() && LevelPatchWanted()) {
        std::lock_guard<std::mutex> lock(g_levelMutex);
        PatchLevel(static_cast<std::uint8_t*>(level));
    }
    return level;
}

// ---- character profiles past 127 -----------------------------------------------------------------------------------
// The game passes a profile index as a signed char (NetPlayerInfo +0x6019, the player's +0x31, the saves, the levels'
// player lists all keep a byte), but it reads every profile through UIInfo::GetCharacterProfile (and
// GetFirstPlayerProfile / GetFirstPlayerProfileIndex) and every caller hands it the byte zero-extended. Those three
// read it as unsigned here, so profiles 128 to 254 can be worn (0xFF stays "none": the count is a byte, so 0xFF is
// never below it); NetPlayerInfo::RespawnAuthoritativePlayer, which sets the profile the campsite changes to, takes
// any index but 0xFF (it skipped the negative ones). The other users compare the byte with 0xFF only, or pass it on.
struct CodeEdit {
    std::uintptr_t rva;
    std::uint8_t length;
    std::uint8_t original[4];
    std::uint8_t replacement[4];
};
// UIInfo::GetCharacterProfile(signed char): test cl, cl; js null; mov r8, [rip+...]; movsx eax, cl; movzx edx, byte
// [r8+0x18]; cmp eax, edx; jge null; movsx rax, cl; imul rax, rax, 0xA8; add rax, [r8+0x10]; ret
constexpr std::uintptr_t kRvaGetCharacterProfile = 0x8B3890;
constexpr std::uint8_t kGetCharacterProfileSig[] = {0x84, 0xC9, 0x78, 0x23, 0x4C, 0x8B, 0x05, 0x85, 0xAE, 0x56, 0x02,
                                                    0x0F, 0xBE, 0xC1, 0x41, 0x0F, 0xB6, 0x50, 0x18, 0x3B, 0xC2, 0x7D,
                                                    0x10, 0x48, 0x0F, 0xBE, 0xC1, 0x48, 0x69, 0xC0, 0xA8, 0x00, 0x00,
                                                    0x00, 0x49, 0x03, 0x40, 0x10, 0xC3, 0x33, 0xC0, 0xC3};
// UIInfo::GetFirstPlayerProfile / GetFirstPlayerProfileIndex: the same read inlined in a loop from 0 (+0x13: test al,
// al; js; movsx ecx, al; cmp ecx, r8d; jge; movsx rcx, al).
constexpr std::uintptr_t kRvaGetFirstPlayerProfile = 0x8B3AE0;
constexpr std::uintptr_t kRvaGetFirstPlayerProfileIndex = 0x8B3B30;
constexpr std::uint8_t kFirstPlayerProfileLoopSig[] = {0x84, 0xC0, 0x78, 0x19, 0x0F, 0xBE, 0xC8, 0x41, 0x3B, 0xC8,
                                                       0x7D, 0x11, 0x48, 0x0F, 0xBE, 0xC8, 0x48, 0x69, 0xD1, 0xA8};
// NetPlayerInfo::RespawnAuthoritativePlayer: cmp r9b, 0xFF; jle (no new profile) -> je.
constexpr std::uintptr_t kRvaRespawnProfileCheck = 0x58D3AE;
constexpr std::uint8_t kRespawnProfileCheckSig[] = {0x41, 0x80, 0xF9, 0xFF, 0x7E, 0x46};
constexpr CodeEdit kProfileEdits[] = {
    {kRvaGetCharacterProfile + 0x02, 2, {0x78, 0x23}, {0x90, 0x90}},                          // js null: gone
    {kRvaGetCharacterProfile + 0x0B, 3, {0x0F, 0xBE, 0xC1}, {0x0F, 0xB6, 0xC1}},              // movzx eax, cl
    {kRvaGetCharacterProfile + 0x17, 4, {0x48, 0x0F, 0xBE, 0xC1}, {0x48, 0x0F, 0xB6, 0xC1}},  // movzx rax, cl
    {kRvaGetFirstPlayerProfile + 0x15, 2, {0x78, 0x19}, {0x90, 0x90}},
    {kRvaGetFirstPlayerProfile + 0x17, 3, {0x0F, 0xBE, 0xC8}, {0x0F, 0xB6, 0xC8}},
    {kRvaGetFirstPlayerProfile + 0x1F, 4, {0x48, 0x0F, 0xBE, 0xC8}, {0x48, 0x0F, 0xB6, 0xC8}},
    {kRvaGetFirstPlayerProfileIndex + 0x15, 2, {0x78, 0x19}, {0x90, 0x90}},
    {kRvaGetFirstPlayerProfileIndex + 0x17, 3, {0x0F, 0xBE, 0xC8}, {0x0F, 0xB6, 0xC8}},
    {kRvaGetFirstPlayerProfileIndex + 0x1F, 4, {0x48, 0x0F, 0xBE, 0xC8}, {0x48, 0x0F, 0xB6, 0xC8}},
    {kRvaRespawnProfileCheck + 0x04, 1, {0x7E}, {0x74}},                                       // jle -> je
};

void InstallProfilePatch() {
    if (!GameBuild::VerifyBytes(kRvaGetCharacterProfile, kGetCharacterProfileSig, sizeof(kGetCharacterProfileSig)) ||
        !GameBuild::VerifyBytes(kRvaGetFirstPlayerProfile + 0x13, kFirstPlayerProfileLoopSig, sizeof(kFirstPlayerProfileLoopSig)) ||
        !GameBuild::VerifyBytes(kRvaGetFirstPlayerProfileIndex + 0x13, kFirstPlayerProfileLoopSig,
                                sizeof(kFirstPlayerProfileLoopSig)) ||
        !GameBuild::VerifyBytes(kRvaRespawnProfileCheck, kRespawnProfileCheckSig, sizeof(kRespawnProfileCheckSig))) {
        Log::Write("CustomOutfits: the character profile code is not what this framework knows: 127 profiles at most");
        return;
    }
    std::size_t done = 0;
    for (; done < std::size(kProfileEdits); ++done) {
        const CodeEdit& e = kProfileEdits[done];
        auto* at = reinterpret_cast<std::uint8_t*>(GameBuild::Base() + e.rva);
        DWORD old = 0;
        if (!VirtualProtect(at, e.length, PAGE_EXECUTE_READWRITE, &old)) break;
        std::memcpy(at, e.replacement, e.length);
        FlushInstructionCache(GetCurrentProcess(), at, e.length);
        VirtualProtect(at, e.length, old, &old);
    }
    if (done < std::size(kProfileEdits)) {   // (put back what was changed)
        for (std::size_t k = 0; k < done; ++k) {
            const CodeEdit& e = kProfileEdits[k];
            auto* at = reinterpret_cast<std::uint8_t*>(GameBuild::Base() + e.rva);
            DWORD old = 0;
            if (VirtualProtect(at, e.length, PAGE_EXECUTE_READWRITE, &old)) {
                std::memcpy(at, e.original, e.length);
                FlushInstructionCache(GetCurrentProcess(), at, e.length);
                VirtualProtect(at, e.length, old, &old);
            }
        }
        Log::Write("CustomOutfits: the character profile code could not be changed: 127 profiles at most");
        return;
    }
    g_maxProfiles = 255;
    Log::Detail("CustomOutfits: character profiles 128 to 254 can be worn (255 profiles)");
}

// (v0.9.455) The campsite's availability hooks (AlwaysAvailable), made whether mods are installed or not.
bool InstallAvailability() {
    if (GameBuild::VerifyBytes(kRvaHasDlcForOutfit, kHasDlcForOutfitSig, sizeof(kHasDlcForOutfitSig)))
        g_retailHasDlcForOutfit = reinterpret_cast<HasDlcForOutfitFn>(GameBuild::Base() + kRvaHasDlcForOutfit);
    const bool unlocked =
        CreateHook(kRvaIsOutfitUnlocked, kIsOutfitUnlockedSig, sizeof(kIsOutfitUnlockedSig),
                   reinterpret_cast<void*>(&HookIsOutfitUnlocked), reinterpret_cast<void**>(&g_origIsOutfitUnlocked),
                   "PlayerProfile::IsOutfitUnlocked");
    const bool has = CreateHook(kRvaHasOutfit, kHasOutfitSig, sizeof(kHasOutfitSig), reinterpret_cast<void*>(&HookHasOutfit),
                                reinterpret_cast<void**>(&g_origHasOutfit), "Crafting::HasOutfit");
    CreateHook(kRvaHasDlcForOutfit, kHasDlcForOutfitSig, sizeof(kHasDlcForOutfitSig),
               reinterpret_cast<void*>(&HookHasDlcForOutfit), reinterpret_cast<void**>(&g_origHasDlcForOutfit),
               "Crafting::HasDLCForOutfit");
    if (unlocked && has && g_retailHasDlcForOutfit)
        Log::Detail("CustomOutfits: every outfit is available at the campsite (a DLC's own outfits: with their DLC)");
    else
        Log::Write("CustomOutfits: the campsite's outfit checks do not match; outfits unlock as in the game");
    return unlocked && has;
}

bool Install() {
    if (!GameBuild::IsSupportedRetail()) {
        g_status = "unsupported game build";
        return false;
    }
    EnsureConfigsLoaded();
    // mods' sounds (and the log of the player's sounds, to find the ones to replace)
    const bool sounds = SoundReplace::Install();
    VoiceEdit::Install();   // (outfits' voice settings: their bark chances)
    const bool subtitles = SubtitleEdit::Install();   // (mods' cutscene subtitles)
    const bool available = InstallAvailability();   // (v0.9.455: every outfit available, mods or not)
    if (g_configs.empty() && g_overrides.empty() && g_orders.empty() && g_removed.empty() && !WeaponMods::Any()) {
        g_status = "no custom outfits configured";
        return sounds || subtitles || available;
    }
    const bool hooks =
        CreateHook(kRvaLocalstrGet, kLocalstrGetSig, sizeof(kLocalstrGetSig), reinterpret_cast<void*>(&HookLocalstrGet),
                   reinterpret_cast<void**>(&g_origLocalstrGet), "localstr_get") &&
        g_origIsOutfitUnlocked;   // (InstallAvailability's)
    if (!hooks) {
        // Without these hooks custom outfits would have no names and could not be picked; add nothing.
        MH_RemoveHook(reinterpret_cast<void*>(GameBuild::Base() + kRvaLocalstrGet));
        g_configs.clear();
        g_status = "unavailable (hooks)";
        return false;
    }
    if (!CreateHook(kRvaSetLastOutfitWorn, kSetLastOutfitWornSig, sizeof(kSetLastOutfitWornSig),
                    reinterpret_cast<void*>(&HookSetLastOutfitWorn), reinterpret_cast<void**>(&g_origSetLastOutfitWorn),
                    "PlayerProfile::SetLastOutfitWorn"))
        Log::Detail("CustomOutfits: the custom outfit worn last is not remembered (its toggles follow the first outfit "
                    "wearing the same object)");
    InstallProfilePatch();
    // (character profile slots given on equip: else only the objects that got a slot at the start can be worn)
    g_slotsOnEquip = CreateHook(kRvaEquipSelected, kEquipSelectedSig, sizeof(kEquipSelectedSig),
                                reinterpret_cast<void*>(&HookEquipSelected), reinterpret_cast<void**>(&g_origEquipSelected),
                                "UiMenuStateCampsiteGear::EquipSelectedWeapon");
    if (!g_origHasOutfit || !g_origHasDlcForOutfit)   // (InstallAvailability's)
        Log::Detail("CustomOutfits: custom outfits are only available when their base outfit is");
    // (diagnostics: the campsite pictures as Scaleform loads them; 80 lines at most)
    g_imageLoadHooked =
        CreateHook(kRvaCustomImageLoad, kCustomImageLoadSig, sizeof(kCustomImageLoadSig),
                   reinterpret_cast<void*>(&HookCustomImageLoad), reinterpret_cast<void**>(&g_origCustomImageLoad),
                   "CustomImageLoader::LoadImage");
    if (!g_removed.empty() &&
        !CreateHook(kRvaIsOutfitCraftable, kIsOutfitCraftableSig, sizeof(kIsOutfitCraftableSig),
                    reinterpret_cast<void*>(&HookIsOutfitCraftable), reinterpret_cast<void**>(&g_origIsOutfitCraftable),
                    "UiMenuStateCampsiteGear::IsOutfitCraftable"))
        Log::Detail("CustomOutfits: removed outfits that can be crafted stay in the campsite menu");
    // New object names (custom models) are added to the game's zone name table.
    g_zoneRegistrationAvailable =
        GameBuild::VerifyBytes(kRvaZoneNameMapInsert, kZoneNameMapInsertSig, sizeof(kZoneNameMapInsertSig)) &&
        CreateHook(kRvaClearZoneNames, kClearZoneNamesSig, sizeof(kClearZoneNamesSig),
                   reinterpret_cast<void*>(&HookClearZoneNames), reinterpret_cast<void**>(&g_origClearZoneNames),
                   "ObjectZoneManager::ClearZoneNames");
    if (!g_zoneRegistrationAvailable)
        Log::Write("CustomOutfits: new object names cannot be registered; outfits with new models are unavailable");
    if (!CreateHook(kRvaHostedGraphParams, kHostedGraphParamsSig, sizeof(kHostedGraphParamsSig),
                    reinterpret_cast<void*>(&HookHostedGraphParams), reinterpret_cast<void**>(&g_origHostedGraphParams),
                    "AnimGraphManager::GetHostedGraphParamsByUserAndType"))
        Log::Write("CustomOutfits: new objects get no hosted animation graphs (aiming may stay in the bind pose)");
    if (!CreateHook(kRvaSoundPlexSelectorPick, kSoundPlexSelectorPickSig, sizeof(kSoundPlexSelectorPickSig),
                    reinterpret_cast<void*>(&HookSoundPlexSelectorPick),
                    reinterpret_cast<void**>(&g_origSoundPlexSelectorPick), "SoundPlexSelector::PickBranch"))
        Log::Write("CustomOutfits: new objects get no voice selected by speaker (Lara's grunts may be silent)");
    g_levelPatchReady = InitLevelPatch() &&
                        CreateHook(kRvaGetLevelWithId, kGetLevelWithIdSig, sizeof(kGetLevelWithIdSig),
                                   reinterpret_cast<void*>(&HookGetLevelWithId),
                                   reinterpret_cast<void**>(&g_origGetLevelWithId), "STREAM_GetLevelWithID");
    InstallCustomParts();
    InstallGearMenu();
    InstallGearParts();
    g_status = "waiting for the game's outfit data";
    return true;
}

// The worn outfits' voices for the sound hook, by object: several outfits can wear one object (the one picked last
// wins, as for toggles).
// The outfits whose grunts and foley Lara has, per object she may wear: the outfit wearing it (the one picked last
// among those wearing it), its own grunts / foley (or another outfit's: linked, already copied), else the mod's default
// (none for "<game>"); an object no outfit of the mods wears (the game's own outfits): the defaults.
const OutfitConfig* DefaultConfig(const std::string& key) {
    if (key.empty()) return nullptr;
    for (const auto& c : g_configs)
        if (c.key == key) return &c;
    return nullptr;
}
const OutfitConfig* GruntsOf(const OutfitConfig* c) {
    if (c && c->gruntsGame) return nullptr;
    if (c && (!c->voice.empty() || !c->voiceSettings.Empty())) return c;
    return DefaultConfig(g_defaultGrunts);
}
const OutfitConfig* FoleyOf(const OutfitConfig* c) {
    if (c && c->foleyGame) return nullptr;
    if (c && (!c->foley.empty() || !c->foleySounds.empty())) return c;
    return DefaultConfig(g_defaultFoley);
}

void UpdateVoices() {
    const int last = g_lastWornConfig.load();
    const std::uint16_t worn = Features::CurrentOutfitObjectId();
    // v0.9.338: rebuilt only when who wears what changes (the outfit picked last, the object worn, the configs'
    // objects as they resolve). Every tick, copying and comparing every outfit's sound files (path lists) cost 0.4 ms
    // a tick with a 235-sound voice mod (0.9.337 log: CustomOutfits 680-800 ms per 30 s).
    std::uint64_t key = (static_cast<std::uint64_t>(static_cast<std::uint32_t>(last)) << 32) ^ worn ^
                        (static_cast<std::uint64_t>(g_configs.size()) << 48);
    for (const auto& config : g_configs) key = key * 0x100000001B3ull ^ config.object;
    static std::uint64_t s_key = ~0ull;
    static const OutfitConfig* s_wornGrunts = nullptr;
    if (key != s_key) {
        s_key = key;
        // who wears each object: the last picked first, then the mods' order; the object worn now even when no mod's
        std::map<std::uint32_t, const OutfitConfig*> wearers;
        if (last >= 0 && static_cast<std::size_t>(last) < g_configs.size() && g_configs[last].object)
            wearers[g_configs[last].object] = &g_configs[last];
        for (const auto& config : g_configs)
            if (config.object && !wearers.count(config.object)) wearers[config.object] = &config;
        if (worn && !wearers.count(worn)) wearers[worn] = nullptr;
        std::map<std::uint32_t, std::vector<SoundReplace::Entry>> voices;
        std::map<std::uint32_t, std::vector<SoundReplace::Adjustment>> foley;
        for (const auto& [object, config] : wearers) {
            const OutfitConfig* grunts = GruntsOf(config);
            const OutfitConfig* steps = FoleyOf(config);
            std::vector<SoundReplace::Entry> sounds;
            if (grunts) sounds = grunts->voice;
            if (steps) sounds.insert(sounds.end(), steps->foleySounds.begin(), steps->foleySounds.end());
            if (!sounds.empty()) voices[object] = std::move(sounds);
            if (steps && !steps->foley.empty()) foley[object] = steps->foley;
        }
        SoundReplace::SetVoices(voices);
        SoundReplace::SetAdjustments(foley);
        const auto it = worn ? wearers.find(worn) : wearers.end();
        s_wornGrunts = GruntsOf(it != wearers.end() ? it->second : nullptr);
    }
    // the worn outfit's voice settings
    if (!worn) return;   // (no player right now: keep what is applied)
    const OutfitConfig* grunts = s_wornGrunts;
    if (grunts && !grunts->voiceSettings.Empty()) VoiceEdit::SetWorn(grunts->key, &grunts->voiceSettings);
    else VoiceEdit::SetWorn({}, nullptr);
    VoiceEdit::Tick();
}

void TickGameThread() {
    g_gameThreadId.store(GetCurrentThreadId());
    SoundReplace::Tick();
    const bool nothing = g_configs.empty() && g_overrides.empty() && g_orders.empty() && g_removed.empty() && !WeaponMods::Any();
    if (nothing || !ResolveConfigs()) {
        if (g_configs.empty()) UpdateLaraGear();   // (v0.9.586: Enhancements > Gameplay's gear, without outfit mods too)
        return;
    }
    WeaponMods::Tick([](const std::string& name) { return FindZoneId(name); });   // (v0.9.565)
    UpdateVoices();
    const auto base = GameBuild::Base();
    std::uint8_t* crafting = nullptr;
    std::uint8_t* ui = nullptr;
    if (!CopyGuarded(&crafting, reinterpret_cast<const void*>(base + kRvaGlobalCraftingPtr), sizeof(crafting)) ||
        !CopyGuarded(&ui, reinterpret_cast<const void*>(base + kRvaGlobalUIInfoPtr), sizeof(ui)))
        return;
    if (!crafting || !ui) return;
    ApplyProfiles(ui);
    ApplyOutfits(crafting);
    UpdateZoneAliases();
    PatchLoadedLevels();
    UpdateToggles();
    UpdateLaraGear();
    if (g_activeCount.load() > 0) g_status = "active";
}

const char* WornOutfitName() {
    const int c = g_wornConfig.load();
    if (c < 0 || static_cast<std::size_t>(c) >= g_configs.size()) return nullptr;
    const auto& config = g_configs[static_cast<std::size_t>(c)];
    return config.name.empty() ? config.key.c_str() : config.name.c_str();
}

int WornToggleCount() {
    const int c = g_wornConfig.load();
    if (c < 0 || static_cast<std::size_t>(c) >= g_configs.size()) return 0;
    return static_cast<int>(g_configs[static_cast<std::size_t>(c)].menuToggles.size());   // (v0.9.604: the menu's)
}

bool GetWornToggle(int index, ToggleInfo& out) {
    const int c = g_wornConfig.load();
    if (c < 0 || static_cast<std::size_t>(c) >= g_configs.size()) return false;
    const auto& config = g_configs[static_cast<std::size_t>(c)];
    if (index < 0 || static_cast<std::size_t>(index) >= config.menuToggles.size()) return false;
    const std::size_t t = config.menuToggles[static_cast<std::size_t>(index)];   // (v0.9.604: the menu's toggles)
    std::lock_guard<std::mutex> lock(g_toggleMutex);
    const auto& toggle = config.toggles[t];
    out.name = toggle.title.empty() ? toggle.name.c_str() : toggle.title.c_str();
    out.on = toggle.on;
    out.hides = out.on ? std::string() : ShownNotWith(config, t);
    return true;
}

bool SetWornToggle(int index, bool on, std::vector<int>* hidden) {
    const int c = g_wornConfig.load();
    if (c < 0 || static_cast<std::size_t>(c) >= g_configs.size()) return false;
    auto& config = g_configs[static_cast<std::size_t>(c)];
    if (index < 0 || static_cast<std::size_t>(index) >= config.menuToggles.size()) return false;
    std::vector<std::size_t> others;
    ChangeToggle(config, config.menuToggles[static_cast<std::size_t>(index)], on, &others);   // (v0.9.454)
    if (hidden)   // (v0.9.604: as the menu's indices)
        for (const std::size_t k : others)
            if (const auto at = std::find(config.menuToggles.begin(), config.menuToggles.end(), k); at != config.menuToggles.end())
                hidden->push_back(static_cast<int>(at - config.menuToggles.begin()));
    return true;
}

std::vector<ScriptFolder> ScriptFolders() {
    std::vector<ScriptFolder> folders;
    for (const auto& config : g_configs) {
        if (config.packageFolder.empty()) continue;
        // A mod with several outfits has one folder: its scripts load once, named after the mod.
        const std::string owner = config.mod.empty() ? config.key : config.mod;
        if (std::none_of(folders.begin(), folders.end(), [&](const auto& f) { return f.folder == config.packageFolder; }))
            folders.push_back({owner, config.packageFolder});
    }
    return folders;
}

int ConfiguredCount() { return static_cast<int>(g_configs.size()); }
int ActiveCount() { return g_activeCount.load(); }
const char* Status() { return g_status.c_str(); }
bool HooksImageLoad() { return g_imageLoadHooked; }
void SetImageLoadedListener(ImageLoadedFn listener) { g_imageLoadedListener = listener; }

int RenamedMaterialIds(std::uint32_t id, std::uint32_t* out, int capacity) {
    if (!g_buildDone.load(std::memory_order_acquire)) return 0;
    std::lock_guard<std::mutex> lock(g_buildMutex);
    int count = 0;
    for (const auto& renamed : g_renamed)
        if (renamed.type == 10 && renamed.oldId == id && count < capacity) out[count++] = renamed.newId;
    return count;
}

int ResourceCopies(std::uint8_t type, std::uint32_t id, std::uint32_t* out, int capacity) {
    if (!g_buildDone.load(std::memory_order_acquire)) return 0;
    std::lock_guard<std::mutex> lock(g_buildMutex);
    int count = 0;
    for (const auto* list : {&g_renamed, &g_copied})
        for (const auto& r : *list)
            if (r.type == type && r.oldId == id && count < capacity) out[count++] = r.newId;
    return count;
}

bool CustomIndexRange(unsigned int& first, unsigned int& count) {
    std::lock_guard<std::mutex> lock(g_appliedMutex);
    if (g_applied.empty()) return false;
    first = g_applied.front().listIndex;
    count = static_cast<unsigned int>(g_applied.size());
    return true;
}
}  // namespace CustomOutfits
