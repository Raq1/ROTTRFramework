#pragma once
// The campsite's Outfits list (UiMenuStateCampsiteGear) with more rows than the game's menu holds.
//
// The menu keeps its rows (an index into its sorted outfits per row) in UiMenuStateCampsiteSkills'
// m_traitIndexLookups, a u16[32] at +0x170, and counts them in the signed bytes m_currentNumberOfTraits (+0x1B0) and
// m_maxNumberOfTraits (+0x1B1); the row of the worn outfit's check mark is the signed byte m_indexOfCheckMark
// (+0x1E8). Past 32 rows the game writes over the fields after the array (the game's own 33 outfits already write
// a 33rd row over the count), and from row 40 over the menu's object-load callback (+0x1C0, a vtable pointer the
// outfit change calls through); past 127 the counts turn negative.
//
// Here (framework 0.9.401; it replaces 0.9.310's fix, which kept rows 32..127 aside but left the signed counts):
// rows 32 and up go to a table of the framework's (every writer of the Gear menu's rows, and every reader of the rows
// in the campsite menus' code, go through relays); the Gear menu's code counts its rows in two ints placed in the
// padding after UiMenuStateCampsiteSkills' last field (+0x1B8 rows, +0x1BC rows considered), and keeps its check mark
// row in an int at +0x1E8 (the byte and the padding after it). The patches are put in place when the Gear menu is
// first entered (UiMenuStateCampsiteGear::OnEnter's hook, which also sets the ints from the bytes).

namespace OutfitMenuRows {
// Checks the game's code and prepares the patches (Features::CreateHooks). false: the list stays the game's.
bool Install();
// Rows the Outfits list can hold (outfits that can be worn or crafted, then the broken outfits' parts): the game's
// 32 when the patches are not available.
unsigned int Capacity();
}  // namespace OutfitMenuRows
