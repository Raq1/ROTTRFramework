#pragma once
#include "outfit_archive.h"
#include "nlohmann/json.hpp"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

// (v0.9.565) Mods' weapons (the Modding Suite's Weapons tab; mod.json "weapons", "upgrades", "values").
//
// mod.json:
//   "weapons": [ { "object": "wp_pistol",                     a game weapon (its object) changed ...
//                  "name": "...", "description": "...",       (optional) its texts in the menus
//                  "icon": "HandgunMagnum",                   (optional) its icon label (the menus' SymbolLibrary.swf)
//                  "bars": [40, 20, 30, 50, 40, 40] },        (optional) the campsite's six bars: aim speed, damage,
//                                                             recoil, reload speed, ammo capacity, rate of fire
//                { "object": "wp_pistol_gold", "base": "wp_pistol",  ... or a new weapon: a copy of a game weapon's
//                  "folder": "weapons/wp_pistol_gold",        object under a new name (built into the framework archive
//                                                             from base's .drm and the folder's files: its own weapon
//                                                             component, a model), listed in the campsite's weapon list
//                  "unlock": "wp_pistol", ... } ],            once the player has that weapon's trait (default: base's);
//                                                             base's upgrades are its upgrades
//                                                             (v0.9.569) "ui_folder" / "ui_base": its campsite model,
//                                                             <object>_ui built from ui_base (base's wp_*_ui) and the
//                                                             folder's files; the entry's m_uiObjectID is that object
//   "upgrades": [ { "upgrade": 194205, "trait": 247, "preview_trait": 377,   a dtp::WeaponUpgrade's texts and icon:
//                   "name": "...", "description": "...", "icon": "..." } ]   written into its traits and its recipe
//   "values": [ { "dtp": 194205, "offset": 48, "type": "f32", "value": "1.5", "original": "1.3", "size": 220 } ]
//                values of the game's global data (an upgrade's effects, a recipe's costs): DtpEdit, written while the
//                game's value is still "original"
//
// The game's side (retail RVAs; TR2_x64_final names):
//   dtp::GlobalUIInfo* [0x2E1E720]: +0x1060 NumInGameWeapons, +0x1068 m_weaponList (dtp::WeaponUIInfo, 0x50: +0
//     m_inventoryType, +4 m_objectID, +6 m_uiObjectID, +8 m_name, +0xA m_weaponDesc, +0x10 m_FlashIcon, +0x20 m_trait,
//     +0x24 m_traitEndurance, +0x28 dtp::DefaultUIData (6 floats), +0x40 m_isDlc, +0x48 m_dlcRef). The campsite
//     (UiMenuStateCampsiteGear::PopulateSkills) lists an entry of the category's inventory type when the player has its
//     trait (and its DLC); EquipSelectedWeapon removes the items of that type and loads m_objectID
//     (InventoryComponent::LoadItem). The list is replaced by a copy: the game's entries with the mods' changes, then
//     one entry per new weapon (a copy of its base's).
//   GlobalWeaponUpgrades* [0x2D58868] (read at 0x8E6F40): +0 the records' count, +8 the records (0x20: +0 m_objectID,
//     +0x10 the upgrades' count, +0x18 the upgrades): a copy with one record per new weapon (its base's upgrades).
//   GlobalAchievements* [0x2D574A0] (PlayerTraits::GetTrait 0x44D5A0): +0x20 the count, +0x28 dtp::PlayerTrait
//     (0x1C0: +0 m_screenName, +2 m_screenDesc, +0x14 m_craftingRequirement, +0x100 m_iconID).
//   GlobalCrafting* [0x2D57430]: +8 m_craftingRecipeList (dtp::CraftingRecipe 0xC0: +0 name, +2 description).
namespace WeaponMods {
// One mod's mod.json (CustomOutfits::LoadMod); key: the mod's folder name.
void ParseMod(const nlohmann::json& mod, const std::string& key, const std::filesystem::path& folder);
bool Any();

// The texts the mods give (names, descriptions): CustomOutfits gives each a string ID and answers it.
struct Text {
    std::string text;
    int id = -1;
};
std::vector<Text*> Texts();

// The new weapons' objects for the framework archive (CustomOutfits::StartEarly).
void AddSpecs(std::vector<OutfitArchive::OutfitSpec>& specs);

// New weapon object -> its base object (zones; after Tick resolved them), for the animation graph / sound lookups.
std::vector<std::pair<std::uint16_t, std::uint16_t>> Aliases();

// Every tick on the game thread once the new objects' names are registered. findZone: an object's zone ID by name.
void Tick(const std::function<std::uint16_t(const std::string&)>& findZone);
const char* Status();
}  // namespace WeaponMods
