#pragma once
// (v0.9.456) The silencer comes off and goes back on with the weapon's key, as in Tomb Raider (2013).
//
// The game's side (retail RVAs; TR2_x64_final names): a weapon's ammo types are its loadouts (dtp::LoadoutListItem,
// 0x358 bytes, at the weapon's DTP attributes +0x2F8, their count +0x300; WeaponComponent +0x2B8 those attributes).
// A loadout is silenced or not (m_bSilenced +0x58: WeaponComponent::ApplyNPCSoundEventImpact broadcasts no shot for
// the current loadout when set); which loadouts the player may use is said by traits (m_traitsRequired +8, count
// +0x10: all owned; m_traitsToGate +0x18, count +0x20: none owned; OwnerMeetsLoadoutTraitRequirements 0x60D7D0).
// Pressing the key of the weapon in hand again (UiWidgetWeaponSelect::WeaponAndAmmoSelect ->
// InventoryComponent::CycleWeaponMode 0x5C7CD0 -> WeaponComponent::CycleWeaponLoadoutCursorNext 0x5C7BB0) moves the
// weapon's cursor (WeaponComponent +0x584 + type * 8) to its next usable loadout of the same type.
// A silencer is a trait that the silenced loadout needs and its unsilenced twin is gated by: once crafted, the
// unsilenced loadout can no longer be chosen.
//
// Here: the weapon's pairs are found from its own data (an unsilenced loadout, a silenced one of the same type that
// needs a trait the first is gated by: the silencer; the twin with the same projectiles and the closest debug name
// first). With the silenced loadout in hand, pressing the key again chooses its unsilenced twin ("silencer off"): for
// that weapon the player's loadout checks answer as if the silencer trait were not owned (the game's own code then
// keeps that choice: re-equipping, the ammo reset when switching weapons, a new upgrade's check). Pressing again puts
// the silencer back on and goes on to the next ammo type, as the game would from the silenced loadout. A weapon with
// one ammo type toggles; with more the key goes A silenced, A, B silenced, B, ... A weapon without such a pair (or
// before the silencer is crafted) keeps the game's cycle. The choice is kept per weapon for the session.
// The model (v0.9.457): a weapon's look is its own action graph's (Instance_HasTrait checks of Lara's traits driving
// Instance_DrawGroups nodes). While its silencer is off, that graph's checks of the silencer trait answer "not owned"
// (AGHasTraitNode::ProcessMsg, Trait_HasTrait inside it), and each toggle sends the graph its "Use Base Drawgroups!" and
// "Update Drawgroups!" inputs, so the game draws the weapon as without the silencer; the draw groups the silencer's
// check shows directly are also kept hidden on the model while it is off.
// (v0.9.460) The graphs are those acting on the weapon: every graph of the game (ActionGraphManager's list) whose
// default instance (GetDefaultInstance: the graph's instance or its parent placement) is the weapon's; the in-game
// weapon has none of its own at Instance +0x58.
// The log has each weapon's loadouts (once, when its key is pressed again the first time), the graphs acting on it
// (the inputs, the trait checks, the silencer's draw groups; when none asks for the silencer trait, the graphs that
// do and what they act on), each toggle and the model's draw groups after it.
// ROTTRFramework.ini [Weapons] SilencerToggle (default on), the Weapons tab's checkbox.

#include <cstdint>

namespace Silencer {
bool Install();          // the hooks (before MH_EnableHook(MH_ALL_HOOKS)); false: unavailable (logged)
bool Available();
bool Enabled();
void SetEnabled(bool enabled);   // off: every weapon's silencer back to the game's rules
const char* Status();
// (v0.9.598) A toggle without Lara's reload: the weapon select sends her "loadout_changed_attempt_reload" when the
// loadout in use changed; for a toggle between twins with the same ammo it is not sent (the new loadout is in use at
// once, the clip as it was). [Weapons] SilencerNoReload (default on).
bool NoReloadAvailable();
bool NoReload();
void SetNoReload(bool on);
// (v0.9.606) The weapons drawn without their upgrades' looks: in a weapon's look graph (one with the inputs "Use Base
// Drawgroups!" / "Update Drawgroups!": the family graphs of the bows, axes, pistols, the Hailstorm, rifles and shotguns,
// in game and on the campsite's menu models) the Instance_HasTrait checks of the campsite's upgrades ("wup - <Class> -
// ...": WeaponUpgrade traits of a weapon category, not hidden, not attachments, with draw groups) answer "not owned".
// The suppressors, sights, the grenade launcher and the shotguns' attachments keep their looks; the upgrades' values
// stay. Turned on or off, every weapon look graph is sent its two inputs again (the next tick). The log has the
// upgrades found, the graphs drawn again and the first checks answered. [Weapons] NoUpgradeLooks (default off).
bool PlainLooksAvailable();
bool PlainLooks();
void SetPlainLooks(bool on);
// (v0.9.601) Called with every answer of Trait_HasTraitcheckPersistentAndTransient (this module hooks it): the instance
// asked about, the trait, the answer and the caller's return address. Any thread; keep it short. It may be set before
// Install (it is called only once the hook is in).
using TraitQueryObserver = void (*)(const void* instance, std::uint16_t trait, bool result, std::uintptr_t caller);
void SetTraitQueryObserver(TraitQueryObserver observer);
// The game thread, every frame: a weapon born again (a load) made to look as its silencer is; the model's mask kept.
void TickGameThread();
// (v0.9.462) Game thread, diagnostics for dual wield: the graphs acting on a pistol instance and their kinds of node.
void DescribeGraphsActingOn(const void* instance, const char* label);
// (v0.9.574) Game thread: dual wield's offhand pistol (instance handle; 0: none) next to the weapon it copies (the right
// pistol's instance handle). The graphs acting on the offhand count as that weapon's: with the silencer off their
// checks of its silencer trait answer "not owned" too (the shot's sound and look are chosen there), and each toggle
// sends them the look's inputs. The offhand's instance never becomes the weapon's own (NoteInstance).
void SetCompanionInstance(std::uint64_t companion, std::uint64_t weapon);
}
