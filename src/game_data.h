#pragma once
// The game's own data read straight from its archives: the campsite outfit list (globalcrafting), the localized
// strings (locals.bin), object names (zonelist.ids) and UI images. Used by the Outfit Manager; plain C++.

#include "game_archive.h"

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace GameData {
// dtp::OutfitUnlockType (m_outfitType).
enum UnlockType : std::int32_t { kUnlockNone = 0, kUnlockStory = 1, kUnlockCraftable = 2, kUnlockDlc = 3, kUnlockCommunity = 4 };
const char* UnlockTypeName(std::int32_t type);

// dtp::OutfitsData (0xA8 bytes), one campsite outfit.
struct Outfit {
    int index = 0;                    // position in the outfit list
    std::string debugName;            // m_debugName, e.g. "Witch_Base" (stable: mods refer to retail outfits by it)
    std::int16_t nameId = -1;         // m_name, m_description: string IDs (locals.bin)
    std::int16_t descriptionId = -1;
    std::uint16_t object = 0;         // m_playerObject: zone (object) ID
    std::uint32_t parent = 0xFFFFFFFFu;
    std::string flashIcon;            // m_flashIcon
    std::int32_t unlockType = 0;      // m_outfitType
    std::uint32_t craftingRecipe = 0xFFFFFFFFu;
    std::uint32_t ammoType = 0;
    std::uint16_t traitToUnlock = 0;
    std::uint8_t importFromProfile = 0;
    std::uint32_t dlcHash = 0;        // m_dlcRef
    std::int32_t progressionPriority = 0;
    std::uint32_t communityEventHash = 0;
    std::uint16_t sortOrder = 0;
    std::uint32_t previewResource = 0;   // m_previewImage.m_imageRes
    std::string previewPath;             // m_previewImage.m_filepath, e.g. scaleform\sharedtextures\...\Huntress
    std::int32_t previewX = 0, previewY = 0, previewWidth = 0, previewHeight = 0;
    std::int32_t steamAppId = 0;
};

// The outfit list from globalcrafting.drm. dtpId: the DTP resource holding dtp::GlobalCrafting.
bool ReadOutfits(GameArchive::ArchiveSet& set, std::vector<Outfit>& outfits, std::string& error,
                 std::uint32_t* dtpId = nullptr);
// The same from one DTP resource's data (reference definitions + body), e.g. an extracted .tr10dtp.
bool ParseOutfitList(const std::vector<std::uint8_t>& dtp, std::vector<Outfit>& outfits, std::string& error);

// dtp::PlayerTrait (0x1C0 bytes): a skill or upgrade of Lara's (globalachievements.drm: dtp::GlobalAchievements +0x20
// m_numTraits, +0x28 m_traitsList, as PlayerTraits::GetTrait reads it). Owned, it shows and hides draw groups of the
// player's model: m_drawGroupFlags +0x150 (groups 1-32: bit k is draw group k + 1, cdc::ApplyDrawGroupMask) with
// m_drawGroupFlagsExtended +0x158 (33-64), m_drawGroupFlagsHide +0x154 / +0x15C; m_gatedDrawGroups +0x178 (count
// +0x180; dtp::GatedDrawGroup, 0x28: with the traits it lists also owned, its flags at +0xC / +0x10 / +0x14 / +0x18).
struct Trait {
    int index = 0;
    std::int16_t nameId = -1;         // m_screenName
    std::int32_t category = 0;        // m_traitCategory
    std::int32_t trait = 0;           // m_trait
    bool hidden = false;              // m_hidden
    bool weaponAttachment = false;    // m_isWeaponAttachment (+43)
    std::string debugName;            // mpDebugName (+0x188)
    std::uint64_t show = 0, hide = 0; // draw groups (bit k: group k + 1)
    std::uint64_t gatedShow = 0, gatedHide = 0;   // with other traits as well
};
bool ReadTraits(GameArchive::ArchiveSet& set, std::vector<Trait>& traits, std::string& error, std::uint32_t* dtpId = nullptr);
// dtp::PlayerTraitType (m_trait) and dtp::PlayerTraitCategory (m_traitCategory), as the game's PDB names them.
enum TraitType : std::int32_t { kTraitSkillUpgrade = 3, kTraitWeaponUpgrade = 4, kTraitMultiplayerSkill = 5, kTraitCrafting = 7,
                                kTraitArrowCrafting = 8, kTraitEquipmentCrafting = 9, kTraitOwnership = 10, kTraitOutfits = 11 };
enum TraitCategory : std::int32_t { kCategoryWeaponBow = 4, kCategoryWeaponAxe = 8, kCategoryDlcOutfits = 13 };
const char* TraitTypeName(std::int32_t type);
// Whether a trait's draw groups are Lara's (her model's): not a weapon upgrade's (those are the weapon model's groups:
// UiMenuStateCampsiteWeaponUpgrades::ApplyTraitUpgradesToDrawGroupBits), nor an outfit's, nor multiplayer.
inline bool TraitDrawsOnLara(const Trait& t) {
    if (t.weaponAttachment) return false;
    if (t.trait == kTraitWeaponUpgrade || t.trait == kTraitOutfits || t.trait == kTraitMultiplayerSkill) return false;
    if (t.category >= kCategoryWeaponBow && t.category <= kCategoryWeaponAxe) return false;
    return t.category != kCategoryDlcOutfits;
}
bool ParseTraitList(const std::vector<std::uint8_t>& dtp, std::vector<Trait>& traits, std::string& error);

// locals.bin: string ID -> UTF-8 text ("" for unused IDs).
struct Strings {
    std::vector<std::string> text;
    const char* Get(int id) const {
        return id >= 0 && static_cast<std::size_t>(id) < text.size() ? text[static_cast<std::size_t>(id)].c_str() : "";
    }
};
// set: opened with specialisationMask = kEnglishMask (or with the language archives, ArchiveSet::Open(..., "ENGLISH")).
bool ReadStrings(GameArchive::ArchiveSet& set, Strings& strings, std::string& error);
bool ParseStrings(const std::vector<std::uint8_t>& file, Strings& strings, std::string& error);

// Every copy of a file in the archives, for diagnostics (set opened with a specialisationMask).
std::vector<std::string> DescribeCopies(GameArchive::ArchiveSet& set, const std::string& path);

// The game's file system mask for English text (see ArchiveSet::specialisationMask).
constexpr std::uint32_t kEnglishMask = 0x40000001u;

// zonelist.ids: zone (object) ID -> object name.
bool ReadObjectNames(GameArchive::ArchiveSet& set, std::map<std::uint32_t, std::string>& names, std::string& error);

// A texture (the game's PCD9 resource): DXGI format and the mip chain as stored.
struct Image {
    std::uint32_t format = 0;   // DXGI_FORMAT
    std::uint32_t width = 0, height = 0, mips = 1;
    std::uint32_t highResMips = 0;   // mip levels kept elsewhere (streamed); 0 for UI images
    std::vector<std::uint8_t> data;
};
bool ParseTexture(const std::vector<std::uint8_t>& resource, Image& image, std::string& error);
// The archive path of a UI image's .drm: "pcx64-w\design\image resources\<path>.drm" (lower case).
std::string ImageDrmPath(const std::string& imagePath);
// The first texture of a UI image (Outfit::previewPath).
bool ReadImage(GameArchive::ArchiveSet& set, const std::string& imagePath, Image& image, std::string& error);
}  // namespace GameData
