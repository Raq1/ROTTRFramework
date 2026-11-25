// The silencer off and on again with the weapon's key (see silencer.h). Addresses: the retail Steam ROTTR.exe
// (GameBuild::kSupportedTimestamp), found from TR2_x64_final's functions of the same names; each is checked against
// its first bytes and the layout the code reads against the instructions that read it.
#include "silencer.h"

#include "game_build.h"
#include "log.h"

#include <MinHook.h>
#include <Windows.h>
#include <intrin.h>

#include <atomic>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace Silencer {
namespace {
// ---- the loadouts --------------------------------------------------------------------------------------------------
// InventoryComponent::CycleWeaponMode() -> bool (TR2 0x5FE010; one caller: UiWidgetWeaponSelect::WeaponAndAmmoSelect,
// the weapon's key pressed again). It finds the chosen weapon's WeaponComponent and tail-jumps into:
constexpr std::uintptr_t kRvaCycleWeaponMode = 0x005C7CD0;
// WeaponComponent::CycleWeaponLoadoutCursorNext(LoadoutType, bool) -> bool (TR2 0x5FDEF0): GetNextLoadoutCursor, then
// SetLoadoutCursor with it and the loadout's linked loadout (+0x78) of another type when usable. Its other callers
// (Item::NotifyOwnerCapabilitiesChanged, WeaponComponent::ResetLoadouts) set the cursor to -1 first.
constexpr std::uintptr_t kRvaCycleLoadoutNext = 0x005C7BB0;
// WeaponComponent::GetNextLoadoutCursor(LoadoutType, bool) -> int (TR2 0x61AAA0): the first loadout after the cursor
// (wrapping, the cursor's own last) of the type that CanOwnerUseLoadout(index, true, bool) accepts; -1: none.
constexpr std::uintptr_t kRvaGetNextLoadoutCursor = 0x005E45A0;
// WeaponComponent::SetLoadoutCursor(LoadoutType, short) (TR2 0x66F7F0): changed, it reloads that type's clip
// (ReloadLoadoutCursor) unless that type is the loadout in use.
constexpr std::uintptr_t kRvaSetLoadoutCursor = 0x00638D20;
// WeaponComponent::CanOwnerUseLoadout(index, checkPlayerSelection, bool) -> bool (TR2 0x5F0C10): enableLoadout,
// m_allowPlayerSelection (+0x28), then OwnerMeetsLoadoutTraitRequirements.
constexpr std::uintptr_t kRvaCanOwnerUseLoadout = 0x005BABA0;
// WeaponComponent::OwnerMeetsLoadoutTraitRequirements(index) -> bool (TR2 0x643D10; its callers: CanOwnerUseLoadout,
// InventoryComponent::GetFirstAvailableLoadout / CheckWeaponComponentForCompatibleWeaponForProjectile,
// FindUsableLoadoutMatchingSubCategory, Item::NotifyOwnerCapabilitiesChanged, ValidateInitialLoadout).
constexpr std::uintptr_t kRvaOwnerMeetsTraits = 0x0060D7D0;
// Trait_HasTraitcheckPersistentAndTransient(const Instance*, u16) -> bool (TR2 0x6794F0; false for 0xFFFF)
constexpr std::uintptr_t kRvaHasTrait = 0x00642A60;
// GetLocalPlayerInstance(u8) -> Instance*
constexpr std::uintptr_t kRvaGetLocalPlayer = 0x00403BF0;
constexpr std::uintptr_t kRvaInstancePool = 0x024EEAD0;   // 0x120 each, the handle at +0x118

constexpr std::size_t kWcInstance = 0x08, kWcAttributes = 0x2B8, kWcOwner = 0x330, kWcCursors = 0x584, kCursorStride = 8;
constexpr std::size_t kAttrLoadouts = 0x2F8, kAttrLoadoutCount = 0x300;
constexpr std::size_t kLoadoutSize = 0x358, kLoEnable = 0x00, kLoRequired = 0x08, kLoRequiredCount = 0x10, kLoGate = 0x18,
                      kLoGateCount = 0x20, kLoPlayerSelection = 0x28, kLoProjectiles = 0x30, kLoProjectileCount = 0x38,
                      kLoSilenced = 0x58, kLoItemMode = 0x5C, kLoType = 0x74, kLoLinked = 0x78, kLoIcon = 0x80, kLoDebugName = 0x350;
constexpr std::size_t kInstanceStride = 0x120, kInstanceHandle = 0x118;
constexpr std::uint32_t kMaxInstanceSlots = 30000;
constexpr std::uint16_t kNoTrait = 0xFFFF;
constexpr std::uint32_t kMaxLoadouts = 128, kMaxTraitList = 64;

// ---- the weapon's model ----------------------------------------------------------------------------------------------
// A weapon's look is its action graph's (the family graphs of 0.9.443: the in-game weapon and its menu model share one,
// "UI Weapon?" tells them apart). Instance +0x58 its ActionGraphInstance: +0x78 the nodes (by index), +0x98 their count
// (~ActionGraphInstance 0x71BC80). An ActionGraphNode: vtable, +0x08 its graph, +0x14 the inputs fired (bits), +0x1A /
// +0x1B its input / output counts, before it one pointer per output to its connections (4 bytes each: u16 the target's
// index, u8 its input, u8 flags, high nibble on the last). ActionGraphNode::OutputData(node, output, data, type) (retail
// 0x720CC0, TR2 0x75A090) sends an output (type 2: a signal, no data).
// - The graph's inputs: ActionGraphInstanceInputsNode (vtable 0xD712A0; AGObjectInputsNode 0xD6B1B0 shares its
//   ProcessMsg 0x74C260): its output i is input i; +0x28 its DTP, whose +0 lists them (dtp::ScriptableNodeInputSocketHidden,
//   0x20 each: name +0, type +8). Its listener sends an input with OutputData(node, index, data, 1) (0x749F90). The
//   weapons' graphs have "Use Base Drawgroups!" (every part shown as the base) and "Update Drawgroups!" (the trait checks
//   again).
// - AGHasTraitNode (vtable 0xD75218), ProcessMsg (0x75A430, TR2 0x774A70): on input 2 Trait_HasTraitcheckPersistentAndTransient
//   (the node's instance, trait +0x40); output 2 owned, 3 not, then 1 (the value) and 0 (done).
// - AGInstanceDrawGroups (vtable 0xD75E50), ProcessMsg (0x75E220, TR2 0x77C030): its groups +0x2C (1-32) / +0x30 (33-64),
//   input 5 shows them, 6 hides them, 0 sets the whole mask (+0x28: shown); the mask: Instance::GetDrawableComponent
//   (components +0xD8 [8], count +0xD0 above 8; 0x3DE610), its drawable +0xA0, that one's settings +0xA8, +0x10
//   m_noDrawGroups (bit g - 1: draw group g hidden). Its DTP +0x38 (+0: the caption, inline).
constexpr std::uintptr_t kRvaOutputData = 0x00720CC0, kRvaHasTraitNodeProcessMsg = 0x0075A430,
                         kRvaInputsNodeVtable = 0x00D712A0, kRvaObjectInputsNodeVtable = 0x00D6B1B0,
                         kRvaInputsNodeProcessMsg = 0x0074C260, kRvaHasTraitNodeVtable = 0x00D75218,
                         kRvaDrawGroupsNodeVtable = 0x00D75E50, kRvaDrawGroupsNodeProcessMsg = 0x0075E220;
constexpr std::size_t kInstanceGraph = 0x58, kInstanceComponentCount = 0xD0, kInstanceComponents = 0xD8, kDrawableIndex = 8,
                      kDrawableDefault = 0xA0, kDrawableSettings = 0xA8, kSettingsNoDraw = 0x10;
constexpr std::size_t kGraphNodes = 0x78, kGraphNodeCount = 0x98, kNodeGraph = 0x08,
                      kNodeOutputCount = 0x1B, kNodeInputsList = 0x28, kSocketSize = 0x20, kHasTraitTrait = 0x40,
                      kDrawGroupsLow = 0x2C, kDrawGroupsHigh = 0x30, kDrawGroupsDtp = 0x38;
constexpr int kHasTraitOwned = 2, kHasTraitNotOwned = 3, kDrawGroupsShowInput = 5, kDrawGroupsHideInput = 6;
constexpr int kMaxGraphNodes = 4096;
// (v0.9.460) The in-game weapon has no graph of its own at +0x58 (0.9.459's log: no graph read): a graph acts on the
// instance its nodes resolve "self" to, ActionGraphInstance::GetDefaultInstance (retail 0x3A3700, TR2 0x3C7270): the
// graph's instance (+0xB0 its handle) or, unless the graph's flags +0xC8 have 0x10, that instance's parent placement
// (Instance +0x88). AG_GetInstance (0x71C1B0) gives it for the "self" key; Instance_DrawGroups and Instance_HasTrait
// resolve their instance that way. Every graph: ActionGraphManager::singleton (the pointer at 0x2D9AA50) +0 m_AGList,
// each ActionGraphInstance +8 m_next (ActionGraphManager::Process 0x721840 walks it so); +0xC0 m_graphID (the log).
constexpr std::uintptr_t kRvaGraphManager = 0x02D9AA50, kRvaGetDefaultInstance = 0x003A3700;
constexpr std::size_t kManagerGraphList = 0x00, kGraphNext = 0x08, kGraphId = 0xC0;
constexpr int kMaxGraphsScanned = 100000, kMaxWeaponGraphs = 16;   // (v0.9.574: 16, the offhand's graphs too)

// ---- (v0.9.598) the toggle without Lara's reload -------------------------------------------------------------------
// UiWidgetWeaponSelect::WeaponAndAmmoSelect (retail 0x9D6D40, TR2 0x140A1C660), after CycleWeaponMode and
// UpdateLoadoutFromCursor (the new loadout in use at once: SelectLoadout without its reload), compares the loadout in use
// (WeaponComponent +0x580) with the one before the key (0x9D731F) and, changed, sends Lara's instance (+0x170 -> +0x31A8)
// "loadout_changed_attempt_reload" (0xDD2688) with EventSystem::SendEvent(const char*, Instance*, int, void*, u64)
// (0x7F94D0; the call at 0x9D7355): her state control plays the reload, the game's way to change ammo. A silencer toggle
// changes only the silencer: that one event is not sent when the loadouts before and after hold the same ammo and the
// same attributes (the rule SelectLoadout follows for its own reload, retail 0x62F948..0x62FA03: the projectile of the
// weapon's alt type +0x56C, enabled +0, its alt type +4, 0x48 each, at the loadout's +0x30 / count +0x38, its ammo +0x10;
// the attributes the loadout's own +0xB0 when +0xAC is set, else the weapon's, their first dword). The clip stays as it
// is (both twins are the same LoadoutType: one clip, WeaponComponent +0x588 + type * 8).
constexpr std::uintptr_t kRvaSendEventByName = 0x007F94D0, kRvaReloadEventCall = 0x009D7355, kRvaReloadEventName = 0x00DD2688;
constexpr std::size_t kWcAltType = 0x56C, kLoOverridesAttributes = 0xAC, kLoAttributes = 0xB0, kProjectileSize = 0x48,
                      kProjectileEnable = 0x00, kProjectileAltType = 0x04, kProjectileAmmo = 0x10;

// ---- (v0.9.606) the weapons without their upgrades' looks ----------------------------------------------------------
// A weapon's look is its family graph's (bows, axes, pistols, the Hailstorm, rifles, shotguns: 0.9.443): "Use Base
// Drawgroups!" shows the base, "Update Drawgroups!" runs its Instance_HasTrait checks of the campsite's upgrades
// ("wup - <Class> - ...") into Instance_DrawGroups nodes. With the option on, those checks in a graph that has the two
// inputs answer "not owned" (the HasTrait node's own call, as for the silencer above), so every weapon (in her hands, on
// her back, the campsite's menu models) is drawn as before its first upgrade. Only the looks: the upgrades' values
// (ApplyWeaponUpgrades, retail 0x5B37A0, asks Trait_HasTrait directly) are kept.
// The traits: PlayerTraits::GetTrait (retail 0x44D5A0) is [0x2D574A0] +0x28 + i * 0x1C0, the count at +0x20. A
// dtp::PlayerTrait: +4 m_traitCategory (WeaponUpgrade_Bow 4 .. _Axe 8), +8 m_trait (WeaponUpgrade 4), +0x28 m_hidden,
// +0x2B m_isWeaponAttachment, +0x150 / +0x154 / +0x158 / +0x15C the draw groups it shows and hides, +0x180 its gated
// groups' count, +0x188 its debug name. Taken: WeaponUpgrade, a weapon category, not hidden (the story's arrows), not an
// attachment (the suppressors, the laser sight, the grenade launcher, the shotguns' attachments, the pistol's sight and
// taser keep their looks), with draw groups of its own, named "wup - ..." (globalachievements.drm, read with the Suite's
// GameData::ReadTraits: 92 of 969 traits, the bows' 20, the shotguns' 22, the rifles' 27, the pistols' 19, the axe's 4;
// "wup bow explosive arrow" is the one weapon upgrade with draw groups left out by its name).
constexpr std::uintptr_t kRvaPlayerTraitsGetTrait = 0x0044D5A0, kRvaPlayerTraits = 0x02D574A0;
constexpr std::size_t kTraitsCount = 0x20, kTraitsList = 0x28, kTraitSize = 0x1C0, kTraitCategory = 0x04, kTraitType = 0x08,
                      kTraitHidden = 0x28, kTraitAttachment = 0x2B, kTraitShow = 0x150, kTraitHide = 0x154, kTraitShowHigh = 0x158,
                      kTraitHideHigh = 0x15C, kTraitGatedCount = 0x180, kTraitDebugName = 0x188;
constexpr std::int32_t kTraitWeaponUpgrade = 4, kCategoryBow = 4, kCategoryAxe = 8;
constexpr std::uint32_t kMaxPlainTraits = 4096;

struct Check {
    std::uintptr_t rva;
    std::uint8_t bytes[32];
    std::size_t size;
    const char* what;
};
const Check kChecks[] = {
    {kRvaCycleWeaponMode, {0x48, 0x89, 0x5C, 0x24, 0x08, 0x57, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x83, 0xB9, 0xC8, 0x02, 0x00, 0x00, 0x00}, 18,
     "InventoryComponent::CycleWeaponMode"},
    {kRvaCycleLoadoutNext, {0x48, 0x89, 0x5C, 0x24, 0x18, 0x48, 0x89, 0x74, 0x24, 0x20, 0x41, 0x56, 0x48, 0x83, 0xEC, 0x20, 0x45, 0x0F, 0xB6, 0xF0}, 20,
     "WeaponComponent::CycleWeaponLoadoutCursorNext"},
    {kRvaGetNextLoadoutCursor, {0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x6C, 0x24, 0x10, 0x48, 0x89, 0x74, 0x24, 0x18, 0x48, 0x89, 0x7C, 0x24, 0x20}, 20,
     "WeaponComponent::GetNextLoadoutCursor"},
    {kRvaSetLoadoutCursor, {0x40, 0x53, 0x55, 0x41, 0x56, 0x41, 0x57, 0x48, 0x83, 0xEC, 0x48, 0x48, 0x89, 0x74, 0x24, 0x70}, 16,
     "WeaponComponent::SetLoadoutCursor"},
    {kRvaCanOwnerUseLoadout, {0x48, 0x89, 0x6C, 0x24, 0x10, 0x48, 0x89, 0x74, 0x24, 0x18, 0x57, 0x48, 0x83, 0xEC, 0x20, 0x4C, 0x8B, 0x99, 0x30, 0x03}, 20,
     "WeaponComponent::CanOwnerUseLoadout"},
    {kRvaOwnerMeetsTraits, {0x40, 0x55, 0x48, 0x83, 0xEC, 0x20, 0x4C, 0x8B, 0x81, 0x30, 0x03, 0x00, 0x00, 0x4D, 0x85, 0xC0, 0x0F, 0x84}, 18,
     "WeaponComponent::OwnerMeetsLoadoutTraitRequirements"},
    {kRvaHasTrait, {0x48, 0x89, 0x6C, 0x24, 0x20, 0x56, 0x48, 0x83, 0xEC, 0x20, 0xB8, 0xFF, 0xFF, 0x00, 0x00, 0x0F, 0xB7, 0xF2}, 18,
     "Trait_HasTraitcheckPersistentAndTransient"},
    {kRvaGetLocalPlayer, {0x40, 0x53, 0x48, 0x83, 0xEC, 0x20, 0x0F, 0xB6, 0xD9}, 9, "GetLocalPlayerInstance"},
    // the layout read here, in GetNextLoadoutCursor and OwnerMeetsLoadoutTraitRequirements
    {0x5E45BE, {0x48, 0x8B, 0x81, 0xB8, 0x02, 0x00, 0x00}, 7, "WeaponComponent +0x2B8 attributes"},
    {0x5E45D1, {0x8B, 0xA8, 0x00, 0x03, 0x00, 0x00}, 6, "attributes +0x300 loadout count"},
    {0x5E45D7, {0x42, 0x0F, 0xBF, 0xBC, 0xF9, 0x84, 0x05, 0x00, 0x00}, 9, "WeaponComponent +0x584 loadout cursors"},
    {0x5E461E, {0x48, 0x69, 0xC8, 0x58, 0x03, 0x00, 0x00}, 7, "LoadoutListItem 0x358 bytes"},
    {0x5E4625, {0x48, 0x03, 0x8A, 0xF8, 0x02, 0x00, 0x00}, 7, "attributes +0x2F8 loadouts"},
    {0x5E462E, {0x44, 0x39, 0x79, 0x74}, 4, "LoadoutListItem +0x74 type"},
    {0x60D875, {0x39, 0x5F, 0x10}, 3, "LoadoutListItem +0x10 required traits' count"},
    {0x60D880, {0x48, 0x8B, 0x47, 0x08}, 4, "LoadoutListItem +0x08 required traits"},
    {0x60D8A3, {0x39, 0x5F, 0x20}, 3, "LoadoutListItem +0x20 gating traits' count"},
    {0x60D8B0, {0x48, 0x8B, 0x47, 0x18}, 4, "LoadoutListItem +0x18 gating traits"},
    {0x5B1BBA, {0x80, 0x78, 0x58, 0x00}, 4, "LoadoutListItem +0x58 m_bSilenced"},
    // the action graphs
    {kRvaOutputData, {0x4C, 0x8B, 0xDC, 0x41, 0x54, 0x41, 0x55, 0x41, 0x57, 0x48, 0x83, 0xEC, 0x50, 0x0F, 0xB6, 0x41, 0x1B}, 17,
     "ActionGraphNode::OutputData (+0x1B outputs)"},
    {0x720D10, {0x49, 0x8B, 0x7F, 0x08, 0x0F, 0xB7, 0x0E, 0x48, 0x8B, 0x5F, 0x78}, 11, "ActionGraphNode +0x08 graph, graph +0x78 nodes"},
    {kRvaHasTraitNodeProcessMsg, {0x40, 0x53, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x8B, 0xD9, 0x85, 0xD2, 0x75, 0x0A}, 13, "AGHasTraitNode::ProcessMsg"},
    {0x75A481, {0x0F, 0xB7, 0x53, 0x40}, 4, "AGHasTraitNode +0x40 trait"},
    {0x71BC8F, {0x8B, 0x99, 0x98, 0x00, 0x00, 0x00}, 6, "ActionGraphInstance +0x98 node count"},
    {0x71F76A, {0x4A, 0x8B, 0x54, 0x23, 0x58}, 5, "Instance +0x58 action graph"},
    {0x74C290, {0x48, 0x8B, 0x46, 0x28, 0x48, 0x8B, 0x10, 0x0F, 0xB6, 0x44, 0x17, 0x08}, 12, "the graph inputs node's sockets (+0x28, +0; type +8)"},
    {0x75E255, {0x8B, 0x4B, 0x2C, 0x48, 0x89, 0x74, 0x24, 0x30, 0x8B, 0x73, 0x30}, 11, "AGInstanceDrawGroups +0x2C / +0x30 groups"},
    {0x75E26F, {0x48, 0x8B, 0xB8, 0xA0, 0x00, 0x00, 0x00}, 7, "DrawableComponent +0xA0 drawable"},
    {0x75E2D9, {0x48, 0x8B, 0x87, 0xA8, 0x00, 0x00, 0x00, 0x48, 0x09, 0x70, 0x10}, 11, "drawable +0xA8 settings, +0x10 m_noDrawGroups"},
    {0x3DE615, {0x83, 0xB9, 0xD0, 0x00, 0x00, 0x00, 0x08, 0x76, 0x0C, 0x48, 0x8B, 0x81, 0xD8, 0x00, 0x00, 0x00, 0x48, 0x8B, 0x40, 0x40}, 20,
     "Instance::GetDrawableComponent (+0xD8 [8])"},
    // (v0.9.460) the graphs acting on an instance
    {kRvaGetDefaultInstance, {0x4C, 0x8B, 0x81, 0xB0, 0x00, 0x00, 0x00}, 7, "ActionGraphInstance::GetDefaultInstance (+0xB0 instance)"},
    {0x3A3741, {0xF6, 0x81, 0xC8, 0x00, 0x00, 0x00, 0x10}, 7, "GetDefaultInstance's graph flag +0xC8 0x10"},
    {0x3A374A, {0x4C, 0x8B, 0x80, 0x88, 0x00, 0x00, 0x00}, 7, "GetDefaultInstance's Instance +0x88 parent placement"},
    {0x721871, {0x49, 0x8B, 0x04, 0x24}, 4, "ActionGraphManager +0 graph list"},
    {0x721887, {0x48, 0x8B, 0x40, 0x08}, 4, "ActionGraphInstance +8 next graph"},
};
// (v0.9.606) the traits' table (checked apart: when it differs, the weapons keep their upgrades' looks)
const Check kPlainChecks[] = {
    // mov rdx, [rip -> 0x2D574A0]; test rdx, rdx; je; test ecx, ecx; js; cmp ecx, [rdx+0x20]; jae; movsxd rax, ecx;
    // imul rax, rax, 0x1C0
    {kRvaPlayerTraitsGetTrait, {0x48, 0x8B, 0x15, 0xF9, 0x9E, 0x90, 0x02, 0x48, 0x85, 0xD2, 0x74, 0x18, 0x85, 0xC9, 0x78, 0x14,
                                0x3B, 0x4A, 0x20, 0x73, 0x0F, 0x48, 0x63, 0xC1, 0x48, 0x69, 0xC0, 0xC0, 0x01, 0x00, 0x00}, 31,
     "PlayerTraits::GetTrait (its table +0x20 count, 0x1C0 each)"},
};
// (v0.9.598) the toggle without the reload (checked apart: when they differ, the toggle keeps the game's reload)
const Check kNoReloadChecks[] = {
    {kRvaSendEventByName, {0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x74, 0x24, 0x10, 0x57, 0x48, 0x83, 0xEC, 0x40, 0x49, 0x8B, 0xF8, 0x48, 0x8B, 0xF1, 0x4C, 0x8B, 0xC2}, 24,
     "EventSystem::SendEvent (by name)"},
    {0x9D731F, {0x0F, 0xBF, 0x87, 0x80, 0x05, 0x00, 0x00, 0x39, 0x44, 0x24, 0x3C, 0x74}, 12,
     "WeaponAndAmmoSelect's loadout in use (+0x580) against the one before"},
    {0x9D732C, {0x4C, 0x8B, 0x83, 0x70, 0x01, 0x00, 0x00, 0x48, 0x8D, 0x15}, 10, "WeaponAndAmmoSelect's reload event"},
    {kRvaReloadEventName, {'l', 'o', 'a', 'd', 'o', 'u', 't', '_', 'c', 'h', 'a', 'n', 'g', 'e', 'd', '_', 'a', 't', 't', 'e', 'm', 'p', 't', '_',
                           'r', 'e', 'l', 'o', 'a', 'd', 0}, 31, "the event \"loadout_changed_attempt_reload\""},
    {0x62F948, {0x44, 0x8B, 0x8B, 0x6C, 0x05, 0x00, 0x00}, 7, "WeaponComponent +0x56C alt type"},
    {0x62F957, {0x44, 0x8B, 0x45, 0x38}, 4, "LoadoutListItem +0x38 projectile count"},
    {0x62F95D, {0x48, 0x8B, 0x45, 0x30}, 4, "LoadoutListItem +0x30 projectiles"},
    {0x62F96B, {0x38, 0x10}, 2, "ProjectileListItem +0 enabled"},
    {0x62F96F, {0x44, 0x3B, 0x48, 0x04}, 4, "ProjectileListItem +4 alt type"},
    {0x62F979, {0x48, 0x83, 0xC0, 0x48}, 4, "ProjectileListItem 0x48 bytes"},
    {0x62F9C2, {0x80, 0xBD, 0xAC, 0x00, 0x00, 0x00, 0x00, 0x48, 0x8D, 0x8D, 0xB0, 0x00, 0x00, 0x00}, 14,
     "LoadoutListItem +0xAC / +0xB0 its own attributes"},
    {0x62F9F8, {0x8B, 0x40, 0x10, 0x39, 0x42, 0x10}, 6, "ProjectileListItem +0x10 ammo"},
};
struct RelCheck {
    std::uintptr_t rva;
    std::size_t operand, length;
    std::uintptr_t target;
    const char* what;
};
const RelCheck kRelChecks[] = {
    {0x5C7DB3, 1, 5, kRvaCycleLoadoutNext, "CycleWeaponMode's jump to CycleWeaponLoadoutCursorNext"},
    {0x5C7BCC, 1, 5, kRvaGetNextLoadoutCursor, "CycleWeaponLoadoutCursorNext's GetNextLoadoutCursor"},
    {0x5BAC67, 1, 5, kRvaOwnerMeetsTraits, "CanOwnerUseLoadout's OwnerMeetsLoadoutTraitRequirements"},
    {0x60D893, 1, 5, kRvaHasTrait, "OwnerMeetsLoadoutTraitRequirements' Trait_HasTrait"},
    {0x60D7FE, 3, 7, kRvaInstancePool, "OwnerMeetsLoadoutTraitRequirements' instance pool"},
    {0x75A488, 1, 5, kRvaHasTrait, "AGHasTraitNode's Trait_HasTrait"},
    {0x75A4A8, 1, 5, kRvaOutputData, "AGHasTraitNode's OutputData"},
    {0x3A3707, 3, 7, kRvaInstancePool, "GetDefaultInstance's instance pool"},
    {0x3D163D, 3, 7, kRvaGraphManager, "the action graph manager's singleton"},
};
const RelCheck kNoReloadRelChecks[] = {
    {0x9D7333, 3, 7, kRvaReloadEventName, "WeaponAndAmmoSelect's reload event name"},
    {kRvaReloadEventCall, 1, 5, kRvaSendEventByName, "WeaponAndAmmoSelect's SendEvent of the reload"},
};
const RelCheck kPlainRelChecks[] = {   // (v0.9.606)
    {kRvaPlayerTraitsGetTrait, 3, 7, kRvaPlayerTraits, "PlayerTraits::GetTrait's table"},
};
struct VtableCheck {
    std::uintptr_t vtable, function;
    const char* what;
};
const VtableCheck kVtableChecks[] = {   // slot 1 (+8): ProcessMsg
    {kRvaInputsNodeVtable, kRvaInputsNodeProcessMsg, "ActionGraphInstanceInputsNode's vtable"},
    {kRvaObjectInputsNodeVtable, kRvaInputsNodeProcessMsg, "AGObjectInputsNode's vtable"},
    {kRvaHasTraitNodeVtable, kRvaHasTraitNodeProcessMsg, "AGHasTraitNode's vtable"},
    {kRvaDrawGroupsNodeVtable, kRvaDrawGroupsNodeProcessMsg, "AGInstanceDrawGroups' vtable"},
};

using CycleWeaponModeFn = bool(__fastcall*)(void* inventory);
using CycleLoadoutNextFn = bool(__fastcall*)(std::uint8_t* weapon, int type, bool flag);
using GetNextLoadoutFn = int(__fastcall*)(std::uint8_t* weapon, int type, bool flag);
using SetLoadoutCursorFn = void(__fastcall*)(std::uint8_t* weapon, int type, int index);
using CanOwnerUseFn = bool(__fastcall*)(std::uint8_t* weapon, int index, bool checkPlayerSelection, bool flag);
using OwnerMeetsFn = bool(__fastcall*)(std::uint8_t* weapon, int index);
using HasTraitFn = bool(__fastcall*)(const std::uint8_t* instance, std::uint16_t trait);
using GetLocalPlayerFn = std::uint8_t*(__fastcall*)(std::uint8_t index);
using NodeProcessMsgFn = void(__fastcall*)(std::uint8_t* node, int message, void* data);
using OutputDataFn = void(__fastcall*)(std::uint8_t* node, int output, const void* data, int type);
using GetDefaultInstanceFn = std::uint8_t*(__fastcall*)(const std::uint8_t* graph);

CycleWeaponModeFn g_origCycleWeaponMode = nullptr;
CycleLoadoutNextFn g_origCycleLoadoutNext = nullptr;
OwnerMeetsFn g_origOwnerMeets = nullptr;
HasTraitFn g_origHasTrait = nullptr;
NodeProcessMsgFn g_origHasTraitNode = nullptr;
GetNextLoadoutFn g_getNextLoadout = nullptr;
SetLoadoutCursorFn g_setLoadoutCursor = nullptr;
CanOwnerUseFn g_canOwnerUse = nullptr;
GetLocalPlayerFn g_getLocalPlayer = nullptr;
OutputDataFn g_outputData = nullptr;
GetDefaultInstanceFn g_getDefaultInstance = nullptr;
std::uintptr_t g_base = 0;
bool g_installed = false;
std::atomic<bool> g_enabled{true};
const char* g_status = "not installed";
bool g_inUserCycle = false;                        // the game thread: inside CycleWeaponMode (the key pressed again)
// (v0.9.598) the toggle without the reload
using SendEventByNameFn = void(__fastcall*)(void* system, const char* name, void* instance, int size, void* data,
                                            std::uint64_t extra);
SendEventByNameFn g_origSendEventByName = nullptr;
bool g_noReloadInstalled = false;
std::atomic<bool> g_noReload{true};
std::atomic<bool> g_quietPending{false};   // this key press's toggle goes without the reload event
std::atomic<ULONGLONG> g_quietAt{0};
thread_local std::uint16_t t_maskedTrait = kNoTrait;   // inside a masked AGHasTraitNode check: Trait_Has says no
// (v0.9.606) the weapons without their upgrades' looks
bool g_plainInstalled = false;                // the traits' table checked
std::atomic<bool> g_plainLooks{false};        // the option
std::atomic<bool> g_plainRefresh{false};      // every weapon look made again (the game thread's tick)
std::atomic<int> g_plainState{0};             // the upgrades: 0 not read yet, 1 read, -1 none found / unreadable
std::atomic<std::uint64_t> g_plainBits[kMaxPlainTraits / 64]{};   // bit i: trait i is a campsite upgrade's look
std::atomic<std::uint64_t> g_plainAnswered{0};                    // upgrade checks answered "not owned"
std::atomic<int> g_plainLogged{0};
ULONGLONG g_plainLogAt = 0;
std::uint64_t g_plainAnsweredBefore = 0;
thread_local bool t_plainLooks = false;       // inside a weapon look's upgrade check: Trait_Has says no
// IsLookGraph's last answer (per thread: the checks of one graph come one after another)
thread_local const std::uint8_t* t_lookGraph = nullptr;
thread_local std::uint64_t t_lookGraphId = 0;
thread_local std::uint32_t t_lookGraphNodes = 0;
thread_local bool t_lookGraphIs = false;

bool PlainTrait(std::uint16_t trait) {
    return trait < kMaxPlainTraits && ((g_plainBits[trait >> 6].load(std::memory_order_relaxed) >> (trait & 63)) & 1u);
}

constexpr int kMaxWeapons = 16, kMaxPairs = 16, kMaxTraits = 8;
struct Pair {
    std::int16_t plain = -1, silenced = -1;
};
// (v0.9.460) a graph acting on the weapon, as read at a refresh
struct GraphInfo {
    std::uint8_t* graph = nullptr;
    std::uint8_t* inputsNode = nullptr;
    int baseInput = -1, updateInput = -1;
    bool checksSilencer = false;   // one of its Instance_HasTrait nodes asks for the weapon's silencer trait
};
// A weapon seen (its DTP attributes, its loadouts), its pairs and silencer traits, its model's graph. Written on the
// game thread (the atomics also from the loadout check); an entry is rewritten only while it is not "off".
struct Weapon {
    const std::uint8_t* attributes = nullptr;
    const std::uint8_t* loadouts = nullptr;
    std::uint32_t count = 0;
    Pair pairs[kMaxPairs];
    int pairCount = 0;
    std::uint16_t traits[kMaxTraits]{};
    int traitCount = 0;
    std::atomic<bool> off{false};
    // the weapon's model: its instance (handle) and the graph at its +0x58, the last seen (a weapon born again)
    std::atomic<std::uint64_t> instanceHandle{0};
    std::atomic<std::uint8_t*> graph{nullptr};
    std::atomic<bool> refresh{false};   // its look to be made again (the game thread's tick)
    // (v0.9.460) the graphs acting on it (their default instance is the weapon's), found at each refresh; the hook of
    // Instance_HasTrait reads the list from any thread
    std::atomic<std::uint8_t*> graphs[kMaxWeaponGraphs]{};
    std::atomic<int> graphCount{0};
    GraphInfo infos[kMaxWeaponGraphs];
    int infoCount = 0;
    const std::uint8_t* loggedGraphs[kMaxWeaponGraphs]{};   // the graphs already described in the log
    int loggedGraphCount = 0;
    bool loggedTraitGraphs = false;   // the graphs asking for its silencer trait listed (once: none acting on it does)
    std::uint64_t ownedShows = 0, ownedHides = 0, notOwnedShows = 0, notOwnedHides = 0;   // groups (bit g - 1)
    std::uint64_t forcedHidden = 0, forcedShown = 0;   // the bits changed here on the model while "off"
    ULONGLONG logMaskAt = 0;
    std::uint64_t maskBefore = 0;
};
Weapon g_weapons[kMaxWeapons];
std::atomic<int> g_weaponCount{0};
int g_nextReuse = 0;
std::atomic<int> g_offCount{0};

void SetOff(Weapon& w, bool off) {
    if (w.off.exchange(off) != off) g_offCount.fetch_add(off ? 1 : -1);
}

// (v0.9.574) dual wield's offhand pistol and the weapon (instance handle) it copies (SetCompanionInstance)
std::atomic<std::uint64_t> g_companionHandle{0}, g_companionOf{0};
// (v0.9.575) their instances (the pool's slots), for the trait hook on any thread
std::atomic<const std::uint8_t*> g_companionInstance{nullptr}, g_companionOfInstance{nullptr};

const std::uint8_t* Ptr(const std::uint8_t* p, std::size_t offset) {
    return *reinterpret_cast<const std::uint8_t* const*>(p + offset);
}
std::uint8_t* MutablePtr(const std::uint8_t* p, std::size_t offset) {
    return *reinterpret_cast<std::uint8_t* const*>(p + offset);
}
std::uint32_t U32(const std::uint8_t* p, std::size_t offset) {
    return *reinterpret_cast<const std::uint32_t*>(p + offset);
}
std::uint64_t U64(const std::uint8_t* p, std::size_t offset) {
    return *reinterpret_cast<const std::uint64_t*>(p + offset);
}

std::uint8_t* InstanceOf(std::uint64_t handle) {
    if (!handle || static_cast<std::uint32_t>(handle) >= kMaxInstanceSlots) return nullptr;
    auto* instance = reinterpret_cast<std::uint8_t*>(g_base + kRvaInstancePool) +
                     std::size_t(static_cast<std::uint32_t>(handle)) * kInstanceStride;
    return U64(instance, kInstanceHandle) == handle ? instance : nullptr;
}
std::uint8_t* OwnerInstance(const std::uint8_t* weapon) {
    return InstanceOf(U64(weapon, kWcOwner));
}

bool IsLocalPlayerWeapon(const std::uint8_t* weapon) {
    __try {
        const std::uint8_t* owner = OwnerInstance(weapon);
        return owner && owner == g_getLocalPlayer(0);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool Masked(const Weapon& w, std::uint16_t trait) {
    for (int i = 0; i < w.traitCount; ++i)
        if (w.traits[i] == trait) return true;
    return false;
}

// OwnerMeetsLoadoutTraitRequirements with the weapon's silencer traits taken as not owned: 1 / 0, -1 a fault.
int MaskedMeets(const std::uint8_t* weapon, int index, const std::uint8_t* owner, const Weapon& w) {
    __try {
        const std::uint8_t* attributes = Ptr(weapon, kWcAttributes);
        if (!attributes) return 0;
        const int count = static_cast<int>(U32(attributes, kAttrLoadoutCount));
        if (index < 0 || count <= 0) return 0;
        const int at = index < count ? index : count - 1;
        const std::uint8_t* loadouts = Ptr(attributes, kAttrLoadouts);
        if (!loadouts) return 0;
        const std::uint8_t* item = loadouts + std::size_t(at) * kLoadoutSize;
        const auto* required = reinterpret_cast<const std::uint16_t*>(Ptr(item, kLoRequired));
        const std::uint32_t requiredCount = U32(item, kLoRequiredCount);
        for (std::uint32_t i = 0; required && i < requiredCount; ++i) {
            const std::uint16_t trait = required[i];
            if (trait == kNoTrait) continue;
            if (Masked(w, trait) || !g_origHasTrait(owner, trait)) return 0;
        }
        const auto* gate = reinterpret_cast<const std::uint16_t*>(Ptr(item, kLoGate));
        const std::uint32_t gateCount = U32(item, kLoGateCount);
        for (std::uint32_t i = 0; gate && i < gateCount; ++i) {
            const std::uint16_t trait = gate[i];
            if (trait == kNoTrait || Masked(w, trait)) continue;
            if (g_origHasTrait(owner, trait)) return 0;
        }
        return 1;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
}

// The "off" weapon whose attributes these are (and whose loadouts are still those analysed); null: none.
Weapon* OffWeapon(const std::uint8_t* weapon) {
    __try {
        const std::uint8_t* attributes = Ptr(weapon, kWcAttributes);
        if (!attributes) return nullptr;
        const int n = g_weaponCount.load(std::memory_order_acquire);
        for (int i = 0; i < n; ++i) {
            Weapon& w = g_weapons[i];
            if (w.attributes != attributes || !w.off.load(std::memory_order_acquire)) continue;
            if (Ptr(attributes, kAttrLoadouts) != w.loadouts || U32(attributes, kAttrLoadoutCount) != w.count) return nullptr;
            return &w;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    return nullptr;
}

// The weapon's instance and its graph, as seen now (any thread): a new one is made to look right on the next tick.
void NoteInstance(Weapon& w, const std::uint8_t* weapon) {
    __try {
        const std::uint8_t* instance = Ptr(weapon, kWcInstance);
        if (!instance) return;
        const std::uint64_t handle = U64(instance, kInstanceHandle);
        // (v0.9.574) the offhand's own checks (OwnerMeets) leave the weapon's instance the right pistol's
        if (handle && handle == g_companionHandle.load(std::memory_order_acquire)) return;
        std::uint8_t* graph = MutablePtr(instance, kInstanceGraph);
        if (w.instanceHandle.load() != handle || w.graph.load() != graph) {
            w.instanceHandle.store(handle);
            w.graph.store(graph);
            w.refresh.store(true);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

bool __fastcall HookOwnerMeets(std::uint8_t* weapon, int index) {
    const bool game = g_origOwnerMeets(weapon, index);
    if (g_offCount.load(std::memory_order_acquire) <= 0 || !weapon || !g_enabled.load(std::memory_order_relaxed)) return game;
    Weapon* w = OffWeapon(weapon);
    if (!w || !IsLocalPlayerWeapon(weapon)) return game;
    const std::uint8_t* owner = nullptr;
    __try {
        owner = OwnerInstance(weapon);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        owner = nullptr;
    }
    if (!owner) return game;
    NoteInstance(*w, weapon);   // (a weapon born again: a load, a level)
    const int masked = MaskedMeets(weapon, index, owner, *w);
    return masked < 0 ? game : masked != 0;
}

// ---- the model: the trait check in the weapon's own graph --------------------------------------------------------------

// (v0.9.575) Trait_HasTraitcheckPersistentAndTransient answers a weapon from its own CapabilitiesComponent (retail
// TryToGetCapabilitiesComponent 0x1DB5A0; the trait bits at +0x2D8, TR2 +0x2E8; Lara's from her persistent data). The
// offhand pistol's were set once at its birth (SetCapabilitiesForState) and are not the right pistol's: 0.9.574 with
// other pistols, the offhand shot silenced while the right one did not (its silencer mesh hidden only by the right
// pistol's mask). Its trait checks answer as the right pistol's.
// (v0.9.601) Inventory > Equipment's log of the traits the game asks about for Lara (features.cpp)
std::atomic<TraitQueryObserver> g_traitObserver{nullptr};

bool __fastcall HookHasTrait(const std::uint8_t* instance, std::uint16_t trait) {
    if (trait != kNoTrait && trait == t_maskedTrait) return false;
    if (t_plainLooks && PlainTrait(trait)) return false;   // (v0.9.606)
    const std::uint8_t* companion = g_companionInstance.load(std::memory_order_acquire);
    if (instance && instance == companion) {
        const std::uint8_t* of = g_companionOfInstance.load(std::memory_order_acquire);
        __try {
            if (of && U64(companion, kInstanceHandle) == g_companionHandle.load(std::memory_order_acquire) &&
                U64(of, kInstanceHandle) == g_companionOf.load(std::memory_order_acquire))
                instance = of;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
        }
    }
    const bool result = g_origHasTrait(instance, trait);
    if (const TraitQueryObserver observer = g_traitObserver.load(std::memory_order_relaxed))
        observer(instance, trait, result, reinterpret_cast<std::uintptr_t>(_ReturnAddress()));
    return result;
}

// (v0.9.575) a graph acting on dual wield's offhand pistol while it copies the weapon w's instance (found as it asks,
// not only at w's last refresh: another pistol swapped in, or w's look not refreshed since)
bool ActsOnCompanion(const Weapon& w, const std::uint8_t* graph) {
    const std::uint8_t* companion = g_companionInstance.load(std::memory_order_acquire);
    const std::uint64_t of = g_companionOf.load(std::memory_order_acquire);
    if (!companion || !of || !g_getDefaultInstance || w.instanceHandle.load() != of) return false;
    __try {
        return g_getDefaultInstance(graph) == companion;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// (v0.9.460) one of the graphs acting on the weapon
bool ActsOn(const Weapon& w, const std::uint8_t* graph) {
    const int n = w.graphCount.load(std::memory_order_acquire);
    for (int i = 0; i < n && i < kMaxWeaponGraphs; ++i)
        if (w.graphs[i].load(std::memory_order_relaxed) == graph) return true;
    return false;
}

// (v0.9.578) The graph acts on the weapon's instance now (or on dual wield's offhand copying it), its default instance
// read as it asks. A weapon entry keeps its graphs' addresses from its last refresh and stays "off" for the session; a
// pistol replaced (the Weapons list's LoadItem, a reload) and born again has new graphs that may sit at those addresses
// (0.9.577's log: graph 0x...D08F5780 in the Semi-Auto Pistol's list before and after three other pistols). The old
// entry then answered "not owned" to the new pistol's silencer check with the new entry "on": the silencer's mesh
// (group 47) stayed hidden until another pistol was equipped and the Semi-Auto Pistol again.
bool ActsOnNow(const Weapon& w, const std::uint8_t* graph) {
    if (!g_getDefaultInstance || !graph) return false;
    __try {
        const std::uint8_t* weapon = InstanceOf(w.instanceHandle.load());
        if (!weapon) return false;
        const std::uint8_t* acts = g_getDefaultInstance(graph);
        if (acts == weapon || graph == MutablePtr(weapon, kInstanceGraph)) return true;
        const std::uint8_t* companion = g_companionInstance.load(std::memory_order_acquire);
        return companion && acts == companion && w.instanceHandle.load() == g_companionOf.load(std::memory_order_acquire);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// The silencer trait (of an "off" weapon) checked by a graph acting on that weapon: answered "not owned".
// (v0.9.606: called by HookHasTraitNode, below)
void SilencerTraitNode(std::uint8_t* node, int message, void* data) {
    if (message == 2 && node && g_offCount.load(std::memory_order_acquire) > 0 && g_enabled.load(std::memory_order_relaxed)) {
        const std::uint16_t trait = *reinterpret_cast<const std::uint16_t*>(node + kHasTraitTrait);
        const std::uint8_t* graph = Ptr(node, kNodeGraph);
        const int n = g_weaponCount.load(std::memory_order_acquire);
        for (int i = 0; i < n; ++i) {
            const Weapon& w = g_weapons[i];
            if (!w.off.load(std::memory_order_acquire) || !Masked(w, trait)) continue;
            if (!ActsOn(w, graph) && !ActsOnCompanion(w, graph)) continue;
            if (!ActsOnNow(w, graph)) continue;   // (v0.9.578) not a graph of an earlier weapon at the same address
            const std::uint16_t before = t_maskedTrait;
            t_maskedTrait = trait;
            g_origHasTraitNode(node, message, data);
            t_maskedTrait = before;
            return;
        }
    }
    g_origHasTraitNode(node, message, data);
}

std::uint8_t* GraphNode(const std::uint8_t* graph, int index) {
    if (index < 0 || index >= static_cast<int>(U32(graph, kGraphNodeCount))) return nullptr;
    return reinterpret_cast<std::uint8_t* const*>(Ptr(graph, kGraphNodes))[index];
}
// the connections of a node's output (before the node: one pointer per output)
const std::uint8_t* OutputConnections(const std::uint8_t* node, int output) {
    const int outputs = node[kNodeOutputCount];
    if (output < 0 || output >= outputs) return nullptr;
    return *reinterpret_cast<const std::uint8_t* const*>(node - std::ptrdiff_t(outputs - output) * 8);
}
std::uintptr_t VtableRva(const std::uint8_t* node) {
    return *reinterpret_cast<const std::uintptr_t*>(node) - g_base;
}
bool SameName(const char* a, const char* b) {   // case and a trailing '!' aside
    std::size_t i = 0;
    for (; a[i] && b[i]; ++i)
        if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i]))) return false;
    return (!a[i] || (a[i] == '!' && !a[i + 1])) && (!b[i] || (b[i] == '!' && !b[i + 1]));
}
std::uint64_t GroupsOf(const std::uint8_t* drawGroupsNode) {
    return std::uint64_t(U32(drawGroupsNode, kDrawGroupsLow)) | (std::uint64_t(U32(drawGroupsNode, kDrawGroupsHigh)) << 32);
}
int GroupList(std::uint64_t mask, char* out, std::size_t size) {   // "12 33" (draw groups, from 1)
    std::size_t used = 0;
    out[0] = 0;
    for (int g = 0; g < 64; ++g)
        if (mask & (std::uint64_t(1) << g)) {
            const int wrote = std::snprintf(out + used, size - used, "%s%d", used ? " " : "", g + 1);
            if (wrote < 0 || std::size_t(wrote) >= size - used) break;
            used += std::size_t(wrote);
        }
    return int(used);
}

// ---- (v0.9.606) the weapons without their upgrades' looks ----------------------------------------------------------

// A graph's inputs node with its "Use Base Drawgroups!" / "Update Drawgroups!" inputs (-1: not there); null: none.
// The caller guards the reads.
std::uint8_t* LookInputs(const std::uint8_t* graph, int& baseInput, int& updateInput) {
    baseInput = updateInput = -1;
    const int nodes = static_cast<int>(U32(graph, kGraphNodeCount));
    if (nodes < 0 || nodes > kMaxGraphNodes) return nullptr;
    for (int index = 0; index < nodes; ++index) {
        std::uint8_t* node = GraphNode(graph, index);
        if (!node) continue;
        const std::uintptr_t vt = VtableRva(node);
        if (vt != kRvaInputsNodeVtable && vt != kRvaObjectInputsNodeVtable) continue;
        const std::uint8_t* dtp = Ptr(node, kNodeInputsList);
        const std::uint8_t* sockets = dtp ? Ptr(dtp, 0) : nullptr;
        const int outputs = node[kNodeOutputCount];
        for (int i = 0; sockets && i < outputs; ++i) {
            const char* name = reinterpret_cast<const char*>(Ptr(sockets, std::size_t(i) * kSocketSize));
            if (!name) continue;
            if (SameName(name, "Use Base Drawgroups")) baseInput = i;
            if (SameName(name, "Update Drawgroups")) updateInput = i;
        }
        if (baseInput >= 0 || updateInput >= 0) return node;
    }
    return nullptr;
}

// A weapon's look graph: one with "Update Drawgroups!" among its inputs (the family graphs; their menu models' too).
bool IsLookGraph(const std::uint8_t* graph) {
    if (!graph) return false;
    __try {
        const std::uint64_t id = U64(graph, kGraphId);
        const std::uint32_t nodes = U32(graph, kGraphNodeCount);
        if (graph == t_lookGraph && id == t_lookGraphId && nodes == t_lookGraphNodes) return t_lookGraphIs;
        int base = -1, update = -1;
        const bool look = LookInputs(graph, base, update) != nullptr && update >= 0;
        t_lookGraph = graph;
        t_lookGraphId = id;
        t_lookGraphNodes = nodes;
        t_lookGraphIs = look;
        return look;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// A trait's debug name (its record's +0x188), "" when it cannot be read.
const char* PlainTraitName(std::uint16_t trait) {
    __try {
        const std::uint8_t* table = *reinterpret_cast<const std::uint8_t* const*>(g_base + kRvaPlayerTraits);
        if (!table || trait >= U32(table, kTraitsCount)) return "";
        const std::uint8_t* list = Ptr(table, kTraitsList);
        const char* name = list ? reinterpret_cast<const char*>(Ptr(list + std::size_t(trait) * kTraitSize, kTraitDebugName)) : nullptr;
        return name ? name : "";
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return "";
    }
}

// Every AGHasTraitNode's check (input 2): a campsite upgrade's trait checked by a weapon's look graph answers "not owned"
// with the option on; then the silencer's own rule (SilencerTraitNode).
void __fastcall HookHasTraitNode(std::uint8_t* node, int message, void* data) {
    if (message == 2 && node && !t_plainLooks && g_plainLooks.load(std::memory_order_relaxed) &&
        g_plainState.load(std::memory_order_acquire) == 1) {
        std::uint16_t trait = kNoTrait;
        const std::uint8_t* graph = nullptr;
        __try {
            trait = *reinterpret_cast<const std::uint16_t*>(node + kHasTraitTrait);
            graph = Ptr(node, kNodeGraph);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            trait = kNoTrait;
        }
        if (PlainTrait(trait) && IsLookGraph(graph)) {
            g_plainAnswered.fetch_add(1, std::memory_order_relaxed);
            if (g_plainLogged.fetch_add(1, std::memory_order_relaxed) < 12) {
                unsigned long long id = 0;
                __try {
                    id = U64(graph, kGraphId);
                } __except (EXCEPTION_EXECUTE_HANDLER) {
                }
                Log::Writef("Silencer:   no upgrade look: graph %llu's check of trait %u (%s) answered \"not owned\"", id,
                            unsigned(trait), PlainTraitName(trait));
            }
            t_plainLooks = true;
            SilencerTraitNode(node, message, data);
            t_plainLooks = false;
            return;
        }
    }
    SilencerTraitNode(node, message, data);
}

// The campsite's upgrades whose looks are left out (game thread, once the table is loaded): their count; 0 the table
// not loaded yet, -1 none found, -2 a fault.
int ReadPlainTraits(int (&perCategory)[5], int& traitCount) {
    traitCount = 0;
    __try {
        const std::uint8_t* table = *reinterpret_cast<const std::uint8_t* const*>(g_base + kRvaPlayerTraits);
        if (!table) return 0;
        const std::uint32_t count = U32(table, kTraitsCount);
        const std::uint8_t* list = Ptr(table, kTraitsList);
        if (!list || !count) return 0;
        traitCount = int(count);
        int taken = 0;
        for (std::uint32_t i = 0; i < count && i < kMaxPlainTraits; ++i) {
            const std::uint8_t* t = list + std::size_t(i) * kTraitSize;
            const std::int32_t category = *reinterpret_cast<const std::int32_t*>(t + kTraitCategory);
            const std::int32_t type = *reinterpret_cast<const std::int32_t*>(t + kTraitType);
            if (type != kTraitWeaponUpgrade || category < kCategoryBow || category > kCategoryAxe) continue;
            if (t[kTraitHidden] || t[kTraitAttachment]) continue;
            if (!(U32(t, kTraitShow) | U32(t, kTraitHide) | U32(t, kTraitShowHigh) | U32(t, kTraitHideHigh) | U32(t, kTraitGatedCount)))
                continue;
            const char* name = reinterpret_cast<const char*>(Ptr(t, kTraitDebugName));
            if (!name || std::strncmp(name, "wup - ", 6) != 0) continue;
            g_plainBits[i >> 6].fetch_or(std::uint64_t(1) << (i & 63), std::memory_order_relaxed);
            ++perCategory[category - kCategoryBow];
            ++taken;
        }
        return taken ? taken : -1;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -2;
    }
}

// Every weapon look of the game made again (game thread): its graph's "Use Base Drawgroups!", then "Update Drawgroups!"
// (its upgrade checks answering as the option says). The count of graphs told; the game's graph count.
int RefreshLookGraphs(int& scanned, char* ids, std::size_t idsSize) {
    int told = 0;
    scanned = 0;
    std::size_t used = 0;
    ids[0] = 0;
    std::uint64_t seen[16]{};
    int seenCount = 0;
    __try {
        const std::uint8_t* manager = *reinterpret_cast<const std::uint8_t* const*>(g_base + kRvaGraphManager);
        std::uint8_t* graph = manager ? MutablePtr(manager, kManagerGraphList) : nullptr;
        for (; graph && scanned < kMaxGraphsScanned; graph = MutablePtr(graph, kGraphNext), ++scanned) {
            int base = -1, update = -1;
            std::uint8_t* inputs = LookInputs(graph, base, update);
            if (!inputs || update < 0) continue;
            if (base >= 0) g_outputData(inputs, base, nullptr, 2);
            g_outputData(inputs, update, nullptr, 2);
            ++told;
            const std::uint64_t id = U64(graph, kGraphId);
            bool listed = false;
            for (int k = 0; k < seenCount && !listed; ++k) listed = seen[k] == id;
            if (listed || seenCount >= 16) continue;
            seen[seenCount++] = id;
            const int wrote = std::snprintf(ids + used, idsSize - used, "%s%llu", used ? " " : "", static_cast<unsigned long long>(id));
            if (wrote > 0 && std::size_t(wrote) < idsSize - used) used += std::size_t(wrote);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        Log::Write("Silencer: the game's list of action graphs could not be read (the weapons' looks)");
    }
    return told;
}

void TickPlainLooks() {
    if (!g_plainInstalled) return;
    const bool on = g_plainLooks.load(std::memory_order_relaxed);
    static unsigned s_tries = 0;
    if (on && g_plainState.load(std::memory_order_acquire) == 0 && s_tries++ % 30 == 0) {
        int perCategory[5]{}, traitCount = 0;
        const int taken = ReadPlainTraits(perCategory, traitCount);
        if (taken > 0) {
            g_plainState.store(1, std::memory_order_release);
            g_plainRefresh.store(true, std::memory_order_relaxed);
            Log::Writef("Silencer: weapons without their upgrades' looks: %d campsite upgrade(s) of %d traits (bows %d, shotguns "
                        "%d, rifles %d, pistols %d, axes %d); their checks in the weapons' look graphs answer \"not owned\"",
                        taken, traitCount, perCategory[0], perCategory[1], perCategory[2], perCategory[3], perCategory[4]);
        } else if (taken < 0) {
            g_plainState.store(-1, std::memory_order_release);
            Log::Writef("Silencer: weapons without their upgrades' looks: %s; the weapons keep their looks",
                        taken == -1 ? "no campsite upgrade found among the traits" : "the traits could not be read");
        }
    }
    if (g_plainRefresh.load(std::memory_order_relaxed) && g_plainState.load(std::memory_order_acquire) == 1) {
        g_plainRefresh.store(false, std::memory_order_relaxed);
        int scanned = 0;
        char ids[192];
        g_plainAnsweredBefore = g_plainAnswered.load(std::memory_order_relaxed);
        const int told = RefreshLookGraphs(scanned, ids, sizeof(ids));
        Log::Writef("Silencer: weapons %s their upgrades' looks: %d weapon look graph(s) drawn again (of the game's %d; IDs %s)",
                    on ? "without" : "with", told, scanned, ids[0] ? ids : "none");
        g_plainLogAt = GetTickCount64() + 1000;
    }
    if (g_plainLogAt && GetTickCount64() >= g_plainLogAt) {
        g_plainLogAt = 0;
        Log::Writef("Silencer:   %llu upgrade check(s) answered \"not owned\" since",
                    static_cast<unsigned long long>(g_plainAnswered.load(std::memory_order_relaxed) - g_plainAnsweredBefore));
    }
}

// What a graph acting on the weapon says: its inputs node and the two inputs, the trait checks, and the draw group
// nodes the silencer's check drives directly (added to the weapon's masks). Logged the first time (log).
void ReadGraph(Weapon& w, GraphInfo& info, bool log) {
    std::uint8_t* graph = info.graph;
    info.inputsNode = nullptr;
    info.baseInput = info.updateInput = -1;
    info.checksSilencer = false;
    char traits[192] = "", captions[256] = "";
    std::size_t traitsUsed = 0, captionsUsed = 0;
    int nodes = 0, checks = 0;
    __try {
        nodes = static_cast<int>(U32(graph, kGraphNodeCount));
        if (nodes < 0 || nodes > kMaxGraphNodes) nodes = 0;
        for (int index = 0; index < nodes; ++index) {
            std::uint8_t* node = GraphNode(graph, index);
            if (!node) continue;
            const std::uintptr_t vt = VtableRva(node);
            if ((vt == kRvaInputsNodeVtable || vt == kRvaObjectInputsNodeVtable) && !info.inputsNode) {
                const std::uint8_t* dtp = Ptr(node, kNodeInputsList);
                const std::uint8_t* sockets = dtp ? Ptr(dtp, 0) : nullptr;
                const int outputs = node[kNodeOutputCount];
                for (int i = 0; sockets && i < outputs; ++i) {
                    const char* name = reinterpret_cast<const char*>(Ptr(sockets, std::size_t(i) * kSocketSize));
                    if (!name) continue;
                    if (SameName(name, "Use Base Drawgroups")) info.baseInput = i;
                    if (SameName(name, "Update Drawgroups")) info.updateInput = i;
                }
                if (info.baseInput >= 0 || info.updateInput >= 0) info.inputsNode = node;
            } else if (vt == kRvaHasTraitNodeVtable) {
                const std::uint16_t trait = *reinterpret_cast<const std::uint16_t*>(node + kHasTraitTrait);
                ++checks;
                const int wrote = std::snprintf(traits + traitsUsed, sizeof(traits) - traitsUsed, "%s%u", traitsUsed ? "," : "", unsigned(trait));
                if (wrote > 0 && std::size_t(wrote) < sizeof(traits) - traitsUsed) traitsUsed += std::size_t(wrote);
                if (!Masked(w, trait)) continue;
                info.checksSilencer = true;
                for (int output = kHasTraitOwned; output <= kHasTraitNotOwned; ++output) {
                    const std::uint8_t* entry = OutputConnections(node, output);
                    for (int c = 0; entry && c < 64; ++c, entry += 4) {
                        std::uint8_t* target = GraphNode(graph, *reinterpret_cast<const std::uint16_t*>(entry));
                        const int input = entry[2];
                        if (target && VtableRva(target) == kRvaDrawGroupsNodeVtable) {
                            const std::uint64_t groups = GroupsOf(target);
                            std::uint64_t& into = output == kHasTraitOwned ? (input == kDrawGroupsShowInput ? w.ownedShows : w.ownedHides)
                                                                           : (input == kDrawGroupsShowInput ? w.notOwnedShows : w.notOwnedHides);
                            if (input == kDrawGroupsShowInput || input == kDrawGroupsHideInput) into |= groups;
                            const std::uint8_t* dtp = Ptr(target, kDrawGroupsDtp);
                            char caption[48] = "";
                            if (dtp) {
                                std::size_t k = 0;
                                for (; k + 1 < sizeof(caption) && dtp[k] >= 0x20 && dtp[k] < 0x7F; ++k) caption[k] = char(dtp[k]);
                                caption[k] = 0;
                            }
                            char list[96];
                            GroupList(groups, list, sizeof(list));
                            const int put = std::snprintf(captions + captionsUsed, sizeof(captions) - captionsUsed, "%s%s %s \"%s\" [%s]",
                                                          captionsUsed ? "; " : "", output == kHasTraitOwned ? "owned" : "not owned",
                                                          input == kDrawGroupsShowInput ? "shows" : input == kDrawGroupsHideInput ? "hides" : "sets",
                                                          caption, list);
                            if (put > 0 && std::size_t(put) < sizeof(captions) - captionsUsed) captionsUsed += std::size_t(put);
                        } else if (target) {
                            const int put = std::snprintf(captions + captionsUsed, sizeof(captions) - captionsUsed, "%s%s -> node %u (vt 0x%llX) in %d",
                                                          captionsUsed ? "; " : "", output == kHasTraitOwned ? "owned" : "not owned",
                                                          unsigned(*reinterpret_cast<const std::uint16_t*>(entry)),
                                                          static_cast<unsigned long long>(VtableRva(target)), input);
                            if (put > 0 && std::size_t(put) < sizeof(captions) - captionsUsed) captionsUsed += std::size_t(put);
                        }
                        if (entry[3] & 0xF0) break;
                    }
                }
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        Log::Write("Silencer: a graph acting on the weapon could not be read");
    }
    if (!log) return;
    unsigned long long id = 0;
    __try {
        id = U64(graph, kGraphId);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    Log::Writef("Silencer: graph %p (ID %llu) acts on the weapon: %d node(s), inputs \"Use Base Drawgroups!\" %d / \"Update Drawgroups!\" "
                "%d; its %d trait check(s): %s; the silencer's: %s",
                static_cast<const void*>(graph), id, nodes, info.baseInput, info.updateInput, checks, traits[0] ? traits : "none",
                info.checksSilencer ? (captions[0] ? captions : "no draw group node of its own") : "not checked");
}

// (v0.9.460) The graphs acting on the weapon's instance: every graph of the game whose default instance it is (its own
// or a child placement's), and the one at its +0x58 if any. The list for the trait hook; the count of graphs seen.
int FindGraphs(Weapon& w, const std::uint8_t* instance, int& scanned, int* companionGraphs = nullptr) {
    std::uint8_t* found[kMaxWeaponGraphs]{};
    int count = 0;
    scanned = 0;
    // (v0.9.574) dual wield's offhand, when it copies this weapon's instance: its graphs are this weapon's too
    const std::uint8_t* companion = nullptr;
    __try {
        const std::uint64_t of = g_companionOf.load(std::memory_order_acquire);
        if (of && U64(instance, kInstanceHandle) == of) companion = InstanceOf(g_companionHandle.load(std::memory_order_acquire));
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        companion = nullptr;
    }
    int theirs = 0;
    __try {
        const std::uint8_t* manager = *reinterpret_cast<const std::uint8_t* const*>(g_base + kRvaGraphManager);
        std::uint8_t* graph = manager ? MutablePtr(manager, kManagerGraphList) : nullptr;
        for (; graph && scanned < kMaxGraphsScanned; graph = MutablePtr(graph, kGraphNext), ++scanned) {
            if (count >= kMaxWeaponGraphs) continue;
            const std::uint8_t* acts = g_getDefaultInstance(graph);
            if (acts == instance) found[count++] = graph;
            else if (companion && acts == companion) { found[count++] = graph; ++theirs; }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        Log::Write("Silencer: the game's list of action graphs could not be read");
    }
    __try {
        const std::uint8_t* const owners[2] = {instance, companion};
        for (const std::uint8_t* of : owners) {
            if (!of) continue;
            std::uint8_t* own = MutablePtr(of, kInstanceGraph);
            bool listed = own == nullptr;
            for (int i = 0; i < count && !listed; ++i) listed = found[i] == own;
            if (!listed && count < kMaxWeaponGraphs) {
                found[count++] = own;
                if (of == companion) ++theirs;
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    if (companionGraphs) *companionGraphs = companion ? theirs : -1;
    w.graphCount.store(0, std::memory_order_release);
    for (int i = 0; i < count; ++i) w.graphs[i].store(found[i], std::memory_order_relaxed);
    w.graphCount.store(count, std::memory_order_release);
    return count;
}

// (v0.9.460) No graph acting on the weapon asks for its silencer trait: the graphs that do, in the log (once per weapon),
// with the instance each acts on, to find where the weapon's look is made.
void LogTraitGraphs(const Weapon& w, const std::uint8_t* weaponInstance) {
    const std::uint8_t* player = nullptr;
    __try {
        player = g_getLocalPlayer(0);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        player = nullptr;
    }
    int scanned = 0, listed = 0, matching = 0;
    __try {
        const std::uint8_t* manager = *reinterpret_cast<const std::uint8_t* const*>(g_base + kRvaGraphManager);
        std::uint8_t* graph = manager ? MutablePtr(manager, kManagerGraphList) : nullptr;
        for (; graph && scanned < kMaxGraphsScanned; graph = MutablePtr(graph, kGraphNext), ++scanned) {
            int nodes = static_cast<int>(U32(graph, kGraphNodeCount));
            if (nodes < 0 || nodes > kMaxGraphNodes) continue;
            bool asks = false;
            for (int index = 0; index < nodes && !asks; ++index) {
                const std::uint8_t* node = GraphNode(graph, index);
                asks = node && VtableRva(node) == kRvaHasTraitNodeVtable &&
                       Masked(w, *reinterpret_cast<const std::uint16_t*>(node + kHasTraitTrait));
            }
            if (!asks) continue;
            ++matching;
            if (listed >= 8) continue;
            ++listed;
            const std::uint8_t* acts = g_getDefaultInstance(graph);
            const std::uint64_t handle = acts ? U64(acts, kInstanceHandle) : 0;
            Log::Writef("Silencer:   graph %p (ID %llu, %d nodes) asks for the silencer trait; it acts on instance %p (handle %llX)%s",
                        static_cast<const void*>(graph), static_cast<unsigned long long>(U64(graph, kGraphId)), nodes,
                        static_cast<const void*>(acts), static_cast<unsigned long long>(handle),
                        acts == weaponInstance ? ": the weapon" : acts == player ? ": Lara" : !acts ? ": none" : "");
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        Log::Write("Silencer: the game's list of action graphs could not be read");
    }
    Log::Writef("Silencer: no graph acting on the weapon (instance %p) asks for its silencer trait; %d of the game's %d graph(s) "
                "do (above, 8 at most)", static_cast<const void*>(weaponInstance), matching, scanned);
}

std::uint64_t* NoDrawMask(const std::uint8_t* instance) {
    if (U32(instance, kInstanceComponentCount) <= kDrawableIndex) return nullptr;
    const std::uint8_t* components = Ptr(instance, kInstanceComponents);
    const std::uint8_t* drawableComponent = components ? Ptr(components, kDrawableIndex * sizeof(void*)) : nullptr;
    const std::uint8_t* drawable = drawableComponent ? Ptr(drawableComponent, kDrawableDefault) : nullptr;
    std::uint8_t* settings = drawable ? MutablePtr(drawable, kDrawableSettings) : nullptr;
    return settings ? reinterpret_cast<std::uint64_t*>(settings + kSettingsNoDraw) : nullptr;
}

// The model's mask while "off": the groups the silencer's own check shows hidden, those its absence shows drawn (only
// the bits changed here are put back when the silencer goes on again).
void ApplyMask(Weapon& w, const std::uint8_t* instance) {
    __try {
        std::uint64_t* mask = NoDrawMask(instance);
        if (!mask) return;
        const std::uint64_t now = *mask;
        std::uint64_t want = now;
        if (w.off.load()) {
            const std::uint64_t hide = w.ownedShows & ~w.notOwnedShows, show = w.notOwnedShows & ~w.ownedShows;
            want = (now | hide) & ~show;
            w.forcedHidden |= hide & ~now;
            w.forcedShown |= show & now;
        } else {
            want = (now & ~w.forcedHidden) | w.forcedShown;
            w.forcedHidden = w.forcedShown = 0;
        }
        if (want != now) *mask = want;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

// The weapon's look made again (the game thread): its graph's base and update inputs (the trait check answering for the
// silencer as the weapon is now), and the mask.
void Refresh(Weapon& w) {
    const std::uint8_t* instance = nullptr;
    __try {
        instance = InstanceOf(w.instanceHandle.load());
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        instance = nullptr;
    }
    if (!instance) return;
    // (v0.9.460) every graph acting on the weapon (0.9.459 read only the one at the instance's +0x58: none)
    int scanned = 0, companionGraphs = -1;
    const int count = FindGraphs(w, instance, scanned, &companionGraphs);
    w.ownedShows = w.ownedHides = w.notOwnedShows = w.notOwnedHides = 0;
    w.infoCount = 0;
    bool asked = false, fresh = false;
    for (int i = 0; i < count; ++i) {
        GraphInfo& info = w.infos[w.infoCount++];
        info = GraphInfo{};
        info.graph = w.graphs[i].load();
        bool seen = false;
        for (int k = 0; k < w.loggedGraphCount && !seen; ++k) seen = w.loggedGraphs[k] == info.graph;
        if (!seen) {
            fresh = true;
            if (w.loggedGraphCount < kMaxWeaponGraphs) w.loggedGraphs[w.loggedGraphCount++] = info.graph;
            else w.loggedGraphs[kMaxWeaponGraphs - 1] = info.graph;
        }
        ReadGraph(w, info, !seen);
        asked |= info.checksSilencer;
    }
    if (fresh || !count) {
        char offhand[96] = "";
        if (companionGraphs >= 0)   // (v0.9.574)
            std::snprintf(offhand, sizeof(offhand), "; %d of them dual wield's offhand pistol's", companionGraphs);
        Log::Writef("Silencer: %d graph(s) act on the weapon (instance %p; the game has %d%s)", count,
                    static_cast<const void*>(instance), scanned, offhand);
    }
    if (!asked && !w.loggedTraitGraphs) {
        w.loggedTraitGraphs = true;
        LogTraitGraphs(w, instance);
    }
    __try {
        if (std::uint64_t* mask = NoDrawMask(instance)) w.maskBefore = *mask;
        for (int i = 0; i < w.infoCount; ++i) {
            const GraphInfo& info = w.infos[i];
            if (!info.inputsNode) continue;
            if (info.baseInput >= 0) g_outputData(info.inputsNode, info.baseInput, nullptr, 2);
            if (info.updateInput >= 0) g_outputData(info.inputsNode, info.updateInput, nullptr, 2);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        Log::Write("Silencer: the weapon's graph inputs could not be sent");
    }
    ApplyMask(w, instance);
    w.logMaskAt = GetTickCount64() + 500;
}

// ---- the weapon's loadouts ----------------------------------------------------------------------------------------

void CopyName(const std::uint8_t* item, std::size_t offset, char* out, std::size_t size) {
    out[0] = 0;
    __try {
        const char* text = reinterpret_cast<const char*>(Ptr(item, offset));
        if (!text) return;
        std::size_t i = 0;
        for (; i + 1 < size && text[i]; ++i) out[i] = text[i];
        out[i] = 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        out[0] = 0;
    }
}

void LoadoutName(const Weapon& w, int index, char* out, std::size_t size) {
    out[0] = 0;
    if (index >= 0 && std::uint32_t(index) < w.count) CopyName(w.loadouts + std::size_t(index) * kLoadoutSize, kLoDebugName, out, size);
    if (!out[0]) std::snprintf(out, size, "loadout %d", index);
}

void TraitList(const std::uint8_t* item, std::size_t listOffset, std::size_t countOffset, char* out, std::size_t size) {
    const auto* list = reinterpret_cast<const std::uint16_t*>(Ptr(item, listOffset));
    const std::uint32_t count = U32(item, countOffset);
    std::size_t used = 0;
    out[0] = 0;
    for (std::uint32_t i = 0; list && i < count && i < kMaxTraitList; ++i) {
        const int wrote = std::snprintf(out + used, size - used, "%s%u", i ? "," : "", unsigned(list[i]));
        if (wrote < 0 || std::size_t(wrote) >= size - used) break;
        used += std::size_t(wrote);
    }
}

// The weapon's loadouts in the log (once per analysis).
void LogLoadouts(const Weapon& w) {
    __try {
        Log::Detailf("Silencer: weapon attributes %p, %u loadout(s):", static_cast<const void*>(w.attributes), w.count);
        for (std::uint32_t i = 0; i < w.count && i < kMaxLoadouts; ++i) {
            const std::uint8_t* item = w.loadouts + std::size_t(i) * kLoadoutSize;
            char name[64], icon[48], required[96], gate[96];
            CopyName(item, kLoDebugName, name, sizeof(name));
            CopyName(item, kLoIcon, icon, sizeof(icon));
            TraitList(item, kLoRequired, kLoRequiredCount, required, sizeof(required));
            TraitList(item, kLoGate, kLoGateCount, gate, sizeof(gate));
            // (v0.9.572) item mode: the Item's multiple-use mode this loadout sets (WeaponComponent::SelectLoadout)
            Log::Detailf("Silencer:   %u \"%s\": %s, type %d, %s, player choice %d, needs [%s], gated by [%s], projectiles %p x%u, "
                        "linked %d, item mode %d, icon \"%s\"",
                        i, name, item[kLoEnable] ? "enabled" : "disabled", int(U32(item, kLoType)),
                        item[kLoSilenced] ? "SILENCED" : "not silenced", int(item[kLoPlayerSelection]), required, gate,
                        static_cast<const void*>(Ptr(item, kLoProjectiles)), U32(item, kLoProjectileCount),
                        int(U32(item, kLoLinked)), int(U32(item, kLoItemMode)), icon);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        Log::Write("Silencer: the weapon's loadouts could not be read for the log");
    }
}

std::size_t CommonPrefix(const char* a, const char* b) {
    std::size_t n = 0;
    while (a[n] && a[n] == b[n]) ++n;
    return n;
}

bool InList(const std::uint16_t* list, std::uint32_t count, std::uint16_t trait) {
    for (std::uint32_t i = 0; list && i < count && i < kMaxTraitList; ++i)
        if (list[i] == trait) return true;
    return false;
}
// a's traits (0xFFFF and the shared ones aside) all in b and b's in a
bool SameTraits(const std::uint16_t* a, std::uint32_t aCount, const std::uint16_t* b, std::uint32_t bCount, const std::uint16_t* shared,
                int sharedCount) {
    auto skip = [&](std::uint16_t t) {
        if (t == kNoTrait) return true;
        for (int i = 0; i < sharedCount; ++i)
            if (shared[i] == t) return true;
        return false;
    };
    for (std::uint32_t i = 0; a && i < aCount && i < kMaxTraitList; ++i)
        if (!skip(a[i]) && !InList(b, bCount, a[i])) return false;
    for (std::uint32_t i = 0; b && i < bCount && i < kMaxTraitList; ++i)
        if (!skip(b[i]) && !InList(a, aCount, b[i])) return false;
    return true;
}

// The weapon's pairs: an unsilenced loadout and a silenced one of the same type that needs a trait it is gated by.
// The twin first: the same traits but those (the pistol's Hollow Point [7] gated by [243, 246] and Hollow Point
// Silencer [7, 243] gated by [246], not the transient Hollow Point Silencer [155, 243]), then the same projectiles,
// then the closest debug name and place.
bool Analyse(Weapon& w) {
    __try {
        w.pairCount = 0;
        w.traitCount = 0;
        struct Candidate {
            int plain, silenced, score;
        };
        Candidate candidates[kMaxPairs * 4];
        int candidateCount = 0;
        const std::uint32_t count = w.count < kMaxLoadouts ? w.count : kMaxLoadouts;
        for (std::uint32_t u = 0; u < count; ++u) {
            const std::uint8_t* plain = w.loadouts + std::size_t(u) * kLoadoutSize;
            if (!plain[kLoEnable] || plain[kLoSilenced]) continue;
            const auto* gate = reinterpret_cast<const std::uint16_t*>(Ptr(plain, kLoGate));
            const std::uint32_t gateCount = U32(plain, kLoGateCount);
            const auto* plainRequired = reinterpret_cast<const std::uint16_t*>(Ptr(plain, kLoRequired));
            const std::uint32_t plainRequiredCount = U32(plain, kLoRequiredCount);
            if (!gate || !gateCount) continue;
            char plainName[64];
            CopyName(plain, kLoDebugName, plainName, sizeof(plainName));
            for (std::uint32_t s = 0; s < count; ++s) {
                const std::uint8_t* silenced = w.loadouts + std::size_t(s) * kLoadoutSize;
                if (!silenced[kLoEnable] || !silenced[kLoSilenced] || U32(silenced, kLoType) != U32(plain, kLoType)) continue;
                const auto* required = reinterpret_cast<const std::uint16_t*>(Ptr(silenced, kLoRequired));
                const std::uint32_t requiredCount = U32(silenced, kLoRequiredCount);
                std::uint16_t shared[kMaxTraits];
                int sharedCount = 0;
                for (std::uint32_t r = 0; required && r < requiredCount && r < kMaxTraitList; ++r)
                    if (required[r] != kNoTrait && InList(gate, gateCount, required[r]) && sharedCount < kMaxTraits)
                        shared[sharedCount++] = required[r];
                if (!sharedCount || candidateCount >= int(sizeof(candidates) / sizeof(candidates[0]))) continue;
                char silencedName[64];
                CopyName(silenced, kLoDebugName, silencedName, sizeof(silencedName));
                int score = int(CommonPrefix(plainName, silencedName)) * 8 - (u > s ? int(u - s) : int(s - u));
                if (SameTraits(plainRequired, plainRequiredCount, required, requiredCount, shared, sharedCount) &&
                    SameTraits(gate, gateCount, reinterpret_cast<const std::uint16_t*>(Ptr(silenced, kLoGate)), U32(silenced, kLoGateCount),
                               shared, sharedCount))
                    score += 1000000;
                if (Ptr(plain, kLoProjectiles) == Ptr(silenced, kLoProjectiles) &&
                    U32(plain, kLoProjectileCount) == U32(silenced, kLoProjectileCount))
                    score += 100000;
                candidates[candidateCount++] = {int(u), int(s), score};
            }
        }
        // best first; each loadout in one pair
        for (int i = 1; i < candidateCount; ++i)
            for (int j = i; j > 0 && candidates[j].score > candidates[j - 1].score; --j) {
                const Candidate t = candidates[j];
                candidates[j] = candidates[j - 1];
                candidates[j - 1] = t;
            }
        for (int i = 0; i < candidateCount && w.pairCount < kMaxPairs; ++i) {
            bool used = false;
            for (int p = 0; p < w.pairCount && !used; ++p)
                used = w.pairs[p].plain == candidates[i].plain || w.pairs[p].silenced == candidates[i].silenced;
            if (used) continue;
            w.pairs[w.pairCount].plain = static_cast<std::int16_t>(candidates[i].plain);
            w.pairs[w.pairCount].silenced = static_cast<std::int16_t>(candidates[i].silenced);
            ++w.pairCount;
            // the pair's silencer traits: what the silenced one needs and the plain one is gated by
            const std::uint8_t* plain = w.loadouts + std::size_t(candidates[i].plain) * kLoadoutSize;
            const std::uint8_t* silenced = w.loadouts + std::size_t(candidates[i].silenced) * kLoadoutSize;
            const auto* gate = reinterpret_cast<const std::uint16_t*>(Ptr(plain, kLoGate));
            const auto* required = reinterpret_cast<const std::uint16_t*>(Ptr(silenced, kLoRequired));
            for (std::uint32_t r = 0; required && gate && r < U32(silenced, kLoRequiredCount) && r < kMaxTraitList; ++r)
                if (required[r] != kNoTrait && InList(gate, U32(plain, kLoGateCount), required[r]) && !Masked(w, required[r]) &&
                    w.traitCount < kMaxTraits)
                    w.traits[w.traitCount++] = required[r];
        }
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        w.pairCount = 0;
        w.traitCount = 0;
        return false;
    }
}

// The entry of the weapon in hand (analysed now when new or when its loadouts changed); null: unreadable.
Weapon* WeaponOf(const std::uint8_t* weapon) {
    const std::uint8_t* attributes = nullptr;
    const std::uint8_t* loadouts = nullptr;
    std::uint32_t count = 0;
    __try {
        attributes = Ptr(weapon, kWcAttributes);
        if (attributes) {
            loadouts = Ptr(attributes, kAttrLoadouts);
            count = U32(attributes, kAttrLoadoutCount);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
    if (!attributes || !loadouts || !count || count > kMaxLoadouts) return nullptr;
    const int n = g_weaponCount.load(std::memory_order_relaxed);
    Weapon* entry = nullptr;
    bool appended = false;
    for (int i = 0; i < n && !entry; ++i)
        if (g_weapons[i].attributes == attributes) entry = &g_weapons[i];
    if (entry && entry->loadouts == loadouts && entry->count == count) return entry;
    if (!entry) {
        if (n < kMaxWeapons) {
            entry = &g_weapons[n];
            appended = true;
        } else {
            for (int tries = 0; tries < kMaxWeapons && !entry; ++tries) {
                Weapon& candidate = g_weapons[g_nextReuse];
                g_nextReuse = (g_nextReuse + 1) % kMaxWeapons;
                if (!candidate.off.load()) entry = &candidate;
            }
            if (!entry) return nullptr;
        }
    }
    SetOff(*entry, false);
    entry->attributes = attributes;
    entry->loadouts = loadouts;
    entry->count = count;
    entry->graph.store(nullptr);
    entry->instanceHandle.store(0);
    entry->refresh.store(false);
    entry->graphCount.store(0);
    entry->infoCount = 0;
    entry->loggedGraphCount = 0;
    entry->loggedTraitGraphs = false;
    entry->forcedHidden = entry->forcedShown = 0;
    entry->logMaskAt = 0;
    const bool read = Analyse(*entry);
    if (appended) g_weaponCount.store(n + 1, std::memory_order_release);
    LogLoadouts(*entry);
    if (!read) {
        Log::Write("Silencer: the weapon's loadouts could not be read: its key keeps the game's ammo cycle");
    } else if (!entry->pairCount) {
        Log::Detail("Silencer: this weapon has no silenced loadout with an unsilenced twin: its key keeps the game's ammo cycle");
    } else {
        char pairs[160] = "", traits[64] = "";
        std::size_t used = 0;
        for (int i = 0; i < entry->pairCount; ++i) {
            const int wrote = std::snprintf(pairs + used, sizeof(pairs) - used, "%s%d/%d", i ? ", " : "", entry->pairs[i].plain,
                                            entry->pairs[i].silenced);
            if (wrote < 0 || std::size_t(wrote) >= sizeof(pairs) - used) break;
            used += std::size_t(wrote);
        }
        used = 0;
        for (int i = 0; i < entry->traitCount; ++i) {
            const int wrote = std::snprintf(traits + used, sizeof(traits) - used, "%s%u", i ? "," : "", unsigned(entry->traits[i]));
            if (wrote < 0 || std::size_t(wrote) >= sizeof(traits) - used) break;
            used += std::size_t(wrote);
        }
        Log::Writef("Silencer: pairs (unsilenced/silenced) %s; silencer trait(s) %s", pairs, traits);
    }
    return entry;
}

int PlainOf(const Weapon& w, int silenced) {
    for (int i = 0; i < w.pairCount; ++i)
        if (w.pairs[i].silenced == silenced) return w.pairs[i].plain;
    return -1;
}
int SilencedOf(const Weapon& w, int plain) {
    for (int i = 0; i < w.pairCount; ++i)
        if (w.pairs[i].plain == plain) return w.pairs[i].silenced;
    return -1;
}

std::int16_t* CursorOf(std::uint8_t* weapon, int type) {
    return reinterpret_cast<std::int16_t*>(weapon + kWcCursors + std::size_t(type) * kCursorStride);
}

// (v0.9.598) Loadouts a and b hold the same ammo and attributes (SelectLoadout's rule for leaving out its reload, see
// kRvaSendEventByName): the projectile of the weapon's alt type in each, their ammo the same, the attributes' first dword
// the same. why: what differs (for the log).
const std::uint8_t* ProjectileOf(const std::uint8_t* loadout, std::uint32_t altType) {
    const std::uint8_t* list = Ptr(loadout, kLoProjectiles);
    const std::uint32_t count = U32(loadout, kLoProjectileCount);
    for (std::uint32_t i = 0; list && i < count && i < 64; ++i) {
        const std::uint8_t* p = list + std::size_t(i) * kProjectileSize;
        if (p[kProjectileEnable] && U32(p, kProjectileAltType) == altType) return p;
    }
    return nullptr;
}
bool SameAmmo(const std::uint8_t* weapon, const Weapon& w, int a, int b, const char*& why) {
    why = "";
    if (a < 0 || b < 0 || std::uint32_t(a) >= w.count || std::uint32_t(b) >= w.count) {
        why = "a loadout out of the list";
        return false;
    }
    __try {
        const std::uint8_t* la = w.loadouts + std::size_t(a) * kLoadoutSize;
        const std::uint8_t* lb = w.loadouts + std::size_t(b) * kLoadoutSize;
        const std::uint32_t alt = U32(weapon, kWcAltType);
        const std::uint8_t* pa = ProjectileOf(la, alt);
        const std::uint8_t* pb = ProjectileOf(lb, alt);
        if (!pa || !pb) {
            why = "no projectile of the weapon's alt type";
            return false;
        }
        if (U32(pa, kProjectileAmmo) != U32(pb, kProjectileAmmo)) {
            why = "another ammo";
            return false;
        }
        const std::uint8_t* aa = la[kLoOverridesAttributes] ? la + kLoAttributes : w.attributes;
        const std::uint8_t* ab = lb[kLoOverridesAttributes] ? lb + kLoAttributes : w.attributes;
        if (!aa || !ab || U32(aa, 0) != U32(ab, 0)) {
            why = "other attributes";
            return false;
        }
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        why = "unreadable";
        return false;
    }
}

// (v0.9.598) After a toggle from loadout `from` to `to`: this key press's reload event is left out when the two are
// twins with the same ammo.
void ArmQuiet(const std::uint8_t* weapon, const Weapon& w, int from, int to) {
    if (!g_noReloadInstalled || !g_noReload.load(std::memory_order_relaxed) || from == to) return;
    const bool twins = PlainOf(w, from) == to || SilencedOf(w, from) == to;
    const char* why = "not twins";
    if (twins && SameAmmo(weapon, w, from, to, why)) {
        g_quietAt.store(GetTickCount64(), std::memory_order_relaxed);
        g_quietPending.store(true, std::memory_order_release);
        return;
    }
    char a[64], b[64];
    LoadoutName(w, from, a, sizeof(a));
    LoadoutName(w, to, b, sizeof(b));
    Log::Writef("Silencer:   \"%s\" -> \"%s\": the game's reload kept (%s)", a, b, why);
}

// The weapon's other loadout types follow (the pistol's Hollow Point, type 1, to its twin): their cursors to the twin
// usable now.
void FollowOtherTypes(std::uint8_t* weapon, const Weapon& w, int cycledType, bool off) {
    for (int type = 0; type <= 2; ++type) {
        if (type == cycledType) continue;
        const int current = *CursorOf(weapon, type);
        if (current < 0) continue;
        const int twin = off ? PlainOf(w, current) : SilencedOf(w, current);
        if (twin < 0 || !g_canOwnerUse(weapon, twin, true, false)) continue;
        g_setLoadoutCursor(weapon, type, twin);
        char from[64], to[64];
        LoadoutName(w, current, from, sizeof(from));
        LoadoutName(w, twin, to, sizeof(to));
        Log::Writef("Silencer:   type %d too: \"%s\" -> \"%s\"", type, from, to);
    }
}

// The weapon's key pressed again, the player's weapon with pairs: see silencer.h. The game's own
// CycleWeaponLoadoutCursorNext does the change (SetLoadoutCursor, the linked loadout), its cursor put just before the
// loadout chosen here (GetNextLoadoutCursor then finds that one first).
bool Cycle(std::uint8_t* weapon, int type, bool flag) {
    Weapon* w = WeaponOf(weapon);
    if (!w || !w->pairCount || type < 0 || type > 2) return g_origCycleLoadoutNext(weapon, type, flag);
    std::int16_t* cursor = CursorOf(weapon, type);
    const int current = *cursor;
    char from[64], to[64];
    LoadoutName(*w, current, from, sizeof(from));
    bool result = false;
    if (!w->off.load()) {
        const int plain = PlainOf(*w, current);
        if (plain < 0) return g_origCycleLoadoutNext(weapon, type, flag);   // not a silenced twin: the game's cycle
        SetOff(*w, true);
        if (!g_canOwnerUse(weapon, plain, true, false)) {
            SetOff(*w, false);
            LoadoutName(*w, plain, to, sizeof(to));
            Log::Writef("Silencer: \"%s\" not usable: the game's cycle", to);
            return g_origCycleLoadoutNext(weapon, type, flag);
        }
        *cursor = static_cast<std::int16_t>(plain - 1);
        result = g_origCycleLoadoutNext(weapon, type, flag);
        LoadoutName(*w, *cursor, to, sizeof(to));
        Log::Writef("Silencer: off (\"%s\" %d -> \"%s\" %d)", from, current, to, int(*cursor));
        ArmQuiet(weapon, *w, current, *cursor);
    } else {
        SetOff(*w, false);
        const int silenced = SilencedOf(*w, current);
        *cursor = static_cast<std::int16_t>(silenced >= 0 ? silenced : current);
        const int next = g_getNextLoadout(weapon, type, false);
        *cursor = static_cast<std::int16_t>(current);
        if (next < 0 || next == current) {
            result = g_origCycleLoadoutNext(weapon, type, flag);   // (nothing else to go to)
        } else {
            *cursor = static_cast<std::int16_t>(next - 1);
            result = g_origCycleLoadoutNext(weapon, type, flag);
        }
        LoadoutName(*w, *cursor, to, sizeof(to));
        Log::Writef("Silencer: on (\"%s\" %d -> \"%s\" %d)", from, current, to, int(*cursor));
        ArmQuiet(weapon, *w, current, *cursor);   // (its twin: quiet; on to the next ammo type: the game's reload)
    }
    FollowOtherTypes(weapon, *w, type, w->off.load());
    NoteInstance(*w, weapon);
    w->refresh.store(false);
    Refresh(*w);
    return result;
}

bool __fastcall HookCycleLoadoutNext(std::uint8_t* weapon, int type, bool flag) {
    if (!g_inUserCycle) return g_origCycleLoadoutNext(weapon, type, flag);
    g_inUserCycle = false;
    if (!weapon || !g_enabled.load(std::memory_order_relaxed) || !IsLocalPlayerWeapon(weapon))
        return g_origCycleLoadoutNext(weapon, type, flag);
    return Cycle(weapon, type, flag);
}

bool __fastcall HookCycleWeaponMode(void* inventory) {
    g_quietPending.store(false, std::memory_order_relaxed);   // (v0.9.598) armed again by this press's toggle only
    g_inUserCycle = true;
    const bool result = g_origCycleWeaponMode(inventory);
    g_inUserCycle = false;
    return result;
}

// (v0.9.598) EventSystem::SendEvent by name: from WeaponAndAmmoSelect's reload call only (the return address after it),
// armed by this key press's toggle and less than a second ago, the event is not sent. Every other event goes on.
std::uintptr_t g_reloadEventReturn = 0;
void __fastcall HookSendEventByName(void* system, const char* name, void* instance, int size, void* data, std::uint64_t extra) {
    if (reinterpret_cast<std::uintptr_t>(_ReturnAddress()) == g_reloadEventReturn &&
        g_quietPending.exchange(false, std::memory_order_acq_rel)) {
        const ULONGLONG at = g_quietAt.load(std::memory_order_relaxed);
        if (g_noReload.load(std::memory_order_relaxed) && GetTickCount64() - at < 1000) {
            static int logged = 0;
            if (logged++ < 24) Log::Write("Silencer:   no reload (\"loadout_changed_attempt_reload\" not sent to Lara)");
            return;
        }
    }
    g_origSendEventByName(system, name, instance, size, data, extra);
}

template <std::size_t N, std::size_t M>
bool VerifyLists(const Check (&checks)[N], const RelCheck (&rels)[M], const char* outcome) {
    for (const Check& c : checks)
        if (!GameBuild::VerifyBytes(c.rva, c.bytes, c.size)) {
            Log::Writef("Silencer: %s is not where this framework expects it (0x%llX): %s", c.what,
                        static_cast<unsigned long long>(c.rva), outcome);
            return false;
        }
    for (const RelCheck& c : rels) {
        std::int32_t displacement = 0;
        std::memcpy(&displacement, reinterpret_cast<const void*>(g_base + c.rva + c.operand), sizeof(displacement));
        if (c.rva + c.length + std::intptr_t(displacement) != c.target) {
            Log::Writef("Silencer: %s differs (0x%llX): %s", c.what, static_cast<unsigned long long>(c.rva), outcome);
            return false;
        }
    }
    return true;
}

bool Verify() {
    if (!VerifyLists(kChecks, kRelChecks, "the silencer stays on as in the game")) return false;
    for (const VtableCheck& c : kVtableChecks) {
        std::uintptr_t slot = 0;
        std::memcpy(&slot, reinterpret_cast<const void*>(g_base + c.vtable + sizeof(void*)), sizeof(slot));
        if (slot != g_base + c.function) {
            Log::Writef("Silencer: %s differs (0x%llX): the silencer stays on as in the game", c.what, static_cast<unsigned long long>(c.vtable));
            return false;
        }
    }
    return true;
}
}  // namespace

bool Install() {
    if (g_installed) return true;
    g_base = GameBuild::Base();
    if (!g_base || !Verify()) {
        g_status = "unavailable: the game's code differs";
        return false;
    }
    g_getNextLoadout = reinterpret_cast<GetNextLoadoutFn>(g_base + kRvaGetNextLoadoutCursor);
    g_setLoadoutCursor = reinterpret_cast<SetLoadoutCursorFn>(g_base + kRvaSetLoadoutCursor);
    g_canOwnerUse = reinterpret_cast<CanOwnerUseFn>(g_base + kRvaCanOwnerUseLoadout);
    g_getLocalPlayer = reinterpret_cast<GetLocalPlayerFn>(g_base + kRvaGetLocalPlayer);
    g_outputData = reinterpret_cast<OutputDataFn>(g_base + kRvaOutputData);
    g_getDefaultInstance = reinterpret_cast<GetDefaultInstanceFn>(g_base + kRvaGetDefaultInstance);
    struct Hook {
        std::uintptr_t rva;
        void* detour;
        void** original;
    };
    const Hook hooks[] = {
        {kRvaHasTrait, reinterpret_cast<void*>(&HookHasTrait), reinterpret_cast<void**>(&g_origHasTrait)},
        {kRvaOwnerMeetsTraits, reinterpret_cast<void*>(&HookOwnerMeets), reinterpret_cast<void**>(&g_origOwnerMeets)},
        {kRvaCycleLoadoutNext, reinterpret_cast<void*>(&HookCycleLoadoutNext), reinterpret_cast<void**>(&g_origCycleLoadoutNext)},
        {kRvaCycleWeaponMode, reinterpret_cast<void*>(&HookCycleWeaponMode), reinterpret_cast<void**>(&g_origCycleWeaponMode)},
        {kRvaHasTraitNodeProcessMsg, reinterpret_cast<void*>(&HookHasTraitNode), reinterpret_cast<void**>(&g_origHasTraitNode)},
    };
    int made = 0;
    for (const Hook& h : hooks) {
        if (MH_CreateHook(reinterpret_cast<void*>(g_base + h.rva), h.detour, h.original) != MH_OK) break;
        ++made;
    }
    if (made != int(sizeof(hooks) / sizeof(hooks[0]))) {
        for (int i = 0; i < made; ++i) MH_RemoveHook(reinterpret_cast<void*>(g_base + hooks[i].rva));
        g_status = "unavailable: its hooks could not be made";
        Log::Write("Silencer: the hooks could not be made: the silencer stays on as in the game");
        return false;
    }
    g_installed = true;
    g_status = "ready";
    Log::Detail("Silencer: the weapon's key pressed again takes the silencer off and puts it back (Tomb Raider 2013's way)");
    // (v0.9.598) the toggle without Lara's reload
    if (VerifyLists(kNoReloadChecks, kNoReloadRelChecks, "the toggle keeps the game's reload")) {
        if (MH_CreateHook(reinterpret_cast<void*>(g_base + kRvaSendEventByName), reinterpret_cast<void*>(&HookSendEventByName),
                          reinterpret_cast<void**>(&g_origSendEventByName)) == MH_OK) {
            g_reloadEventReturn = g_base + kRvaReloadEventCall + 5;
            g_noReloadInstalled = true;
            Log::Detail("Silencer: a toggle goes without Lara's reload (the ammo the same)");
        } else {
            Log::Write("Silencer: EventSystem::SendEvent's hook could not be made: the toggle keeps the game's reload");
        }
    }
    // (v0.9.606) the weapons without their upgrades' looks (the trait hooks above; the traits' table checked here)
    if (VerifyLists(kPlainChecks, kPlainRelChecks, "the weapons keep their upgrades' looks")) {
        g_plainInstalled = true;
        Log::Detail("Silencer: the weapons can be drawn without their upgrades' looks (Inventory > Weapons)");
    }
    return true;
}

bool PlainLooksAvailable() {
    return g_plainInstalled;
}

bool PlainLooks() {
    return g_plainLooks.load(std::memory_order_relaxed);
}

void SetPlainLooks(bool on) {
    if (g_plainLooks.exchange(on, std::memory_order_relaxed) != on) g_plainRefresh.store(true, std::memory_order_relaxed);
}

bool NoReloadAvailable() {
    return g_noReloadInstalled;
}

void SetTraitQueryObserver(TraitQueryObserver observer) {
    g_traitObserver.store(observer, std::memory_order_release);
}

bool NoReload() {
    return g_noReload.load(std::memory_order_relaxed);
}

void SetNoReload(bool on) {
    g_noReload.store(on, std::memory_order_relaxed);
    if (!on) g_quietPending.store(false, std::memory_order_relaxed);
}

bool Available() {
    return g_installed;
}

bool Enabled() {
    return g_enabled.load(std::memory_order_relaxed);
}

void SetEnabled(bool enabled) {
    g_enabled.store(enabled, std::memory_order_relaxed);
    if (!enabled) {
        const int n = g_weaponCount.load(std::memory_order_relaxed);
        for (int i = 0; i < n; ++i)
            if (g_weapons[i].off.load()) {
                SetOff(g_weapons[i], false);
                g_weapons[i].refresh.store(true);   // (the look back with the next tick)
            }
    }
}

void TickGameThread() {
    if (!g_installed) return;
    TickPlainLooks();   // (v0.9.606)
    static unsigned s_frame = 0;
    ++s_frame;
    const int n = g_weaponCount.load(std::memory_order_acquire);
    const ULONGLONG now = GetTickCount64();
    for (int i = 0; i < n; ++i) {
        Weapon& w = g_weapons[i];
        if (w.refresh.exchange(false)) Refresh(w);
        if (!w.off.load() && !w.forcedHidden && !w.forcedShown && !w.logMaskAt) continue;
        const std::uint8_t* instance = nullptr;
        __try {
            instance = InstanceOf(w.instanceHandle.load());   // (the handle tells a weapon born again apart)
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            instance = nullptr;
        }
        if (!instance) continue;
        if (s_frame % 8 == 0 || !w.off.load()) ApplyMask(w, instance);   // (the game's own changes to the look)
        if (w.logMaskAt && now >= w.logMaskAt) {
            w.logMaskAt = 0;
            std::uint64_t after = 0;
            __try {
                if (const std::uint64_t* mask = NoDrawMask(instance)) after = *mask;
            } __except (EXCEPTION_EXECUTE_HANDLER) {
            }
            char hidden[192], shown[192];
            GroupList(after & ~w.maskBefore, hidden, sizeof(hidden));
            GroupList(w.maskBefore & ~after, shown, sizeof(shown));
            Log::Writef("Silencer: the weapon's draw groups %s: hidden since [%s], shown since [%s] (mask %016llX)", w.off.load() ? "off" : "on",
                        hidden, shown, static_cast<unsigned long long>(after));
        }
    }
}

const char* Status() {
    return g_status;
}
// (v0.9.462) dual wield's diagnostics: the graphs acting on a pistol (their default instance is it), each with its ID,
// flags, node count and its kinds of node (vtable RVAs, how many of each), and the instance's placement keys.
void DescribeGraphsActingOn(const void* instance, const char* label) {
    if (!g_installed || !instance || !g_getDefaultInstance) return;
    std::uint64_t handle = 0, hashKey = 0, parentPlacement = 0;
    __try {
        const auto* i = static_cast<const std::uint8_t*>(instance);
        handle = U64(i, kInstanceHandle);
        hashKey = U64(i, 0x60);
        parentPlacement = U64(i, 0x88);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    Log::Writef("Silencer/dual: %s pistol instance %p handle %llX, placement key %llX, parent placement %llX", label,
                instance, static_cast<unsigned long long>(handle), static_cast<unsigned long long>(hashKey),
                static_cast<unsigned long long>(parentPlacement));
    int scanned = 0, found = 0;
    __try {
        const std::uint8_t* manager = *reinterpret_cast<const std::uint8_t* const*>(g_base + kRvaGraphManager);
        std::uint8_t* graph = manager ? MutablePtr(manager, kManagerGraphList) : nullptr;
        for (; graph && scanned < kMaxGraphsScanned && found < 8; graph = MutablePtr(graph, kGraphNext), ++scanned) {
            if (g_getDefaultInstance(graph) != instance) continue;
            ++found;
            const int nodes = static_cast<int>(U32(graph, kGraphNodeCount));
            std::uintptr_t kinds[48]{};
            int counts[48]{};
            int kindCount = 0;
            for (int n = 0; n < nodes && n < kMaxGraphNodes; ++n) {
                const std::uint8_t* node = GraphNode(graph, n);
                if (!node) continue;
                const std::uintptr_t vt = VtableRva(node);
                int k = 0;
                while (k < kindCount && kinds[k] != vt) ++k;
                if (k == kindCount && kindCount < 48) kinds[kindCount++] = vt;
                if (k < 48) ++counts[k];
            }
            char text[1024]{};
            std::size_t used = 0;
            for (int k = 0; k < kindCount; ++k) {
                const int wrote = std::snprintf(text + used, sizeof(text) - used, "%s%llX x%d", used ? ", " : "",
                                                static_cast<unsigned long long>(kinds[k]), counts[k]);
                if (wrote < 0 || std::size_t(wrote) >= sizeof(text) - used) break;
                used += std::size_t(wrote);
            }
            Log::Writef("Silencer/dual:   graph %p ID %llu, flags %X, its instance %llX, %d nodes: %s", static_cast<void*>(graph),
                        static_cast<unsigned long long>(U64(graph, kGraphId)), U32(graph, 0xC8),
                        static_cast<unsigned long long>(U64(graph, 0xB0)), nodes, text);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        Log::Write("Silencer/dual: the game's list of action graphs could not be read");
    }
    Log::Writef("Silencer/dual: %s pistol: %d graph(s) act on it (the game has %d)", label, found, scanned);
}

// (v0.9.574) see silencer.h: the weapons copied (before or now) are looked at again on the next tick (Refresh: their
// graphs found with the offhand's, the look's inputs sent to them too).
void SetCompanionInstance(std::uint64_t companion, std::uint64_t weapon) {
    if (!companion || !weapon) companion = weapon = 0;
    const std::uint64_t before = g_companionHandle.exchange(companion, std::memory_order_acq_rel);
    const std::uint64_t beforeOf = g_companionOf.exchange(weapon, std::memory_order_acq_rel);
    const std::uint8_t* companionInstance = nullptr;
    const std::uint8_t* ofInstance = nullptr;
    __try {
        companionInstance = companion ? InstanceOf(companion) : nullptr;
        ofInstance = weapon ? InstanceOf(weapon) : nullptr;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        companionInstance = ofInstance = nullptr;
    }
    g_companionOfInstance.store(companionInstance && ofInstance ? ofInstance : nullptr, std::memory_order_release);
    g_companionInstance.store(companionInstance && ofInstance ? companionInstance : nullptr, std::memory_order_release);
    if (before == companion && beforeOf == weapon) return;
    int marked = 0;
    const int n = g_weaponCount.load(std::memory_order_acquire);
    for (int i = 0; i < n; ++i) {
        const std::uint64_t handle = g_weapons[i].instanceHandle.load();
        if (handle && companion && handle == companion) {   // noted from the offhand before it was known as one
            g_weapons[i].instanceHandle.store(weapon);
            g_weapons[i].graph.store(nullptr);
            g_weapons[i].refresh.store(true);
            ++marked;
        } else if (handle && (handle == weapon || handle == beforeOf)) {
            g_weapons[i].refresh.store(true);
            ++marked;
        }
    }
    if (companion)
        Log::Detailf("Silencer: dual wield's offhand pistol (handle %llX) answers the silencer as the weapon it copies "
                    "(handle %llX; %d weapon entr%s to look at again)", static_cast<unsigned long long>(companion),
                    static_cast<unsigned long long>(weapon), marked, marked == 1 ? "y" : "ies");
    else
        Log::Detail("Silencer: no dual wield offhand pistol any more");
}

}  // namespace Silencer
