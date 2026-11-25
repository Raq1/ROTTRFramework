#pragma once
#include <cstdint>

// The game's own ObjectZone name table (replaces the old embedded copy of zonelist.ids).
//
// Retail facts (Steam ROTTR.exe 0x632D2D99):
//   The game loads "<data>\zonelist.ids" once during startup (RVA 0x2465D0, called from the
//   one-time game init at RVA 0x4582E0 through RVA 0x421970) into the ObjectZoneManager
//   singleton (pointer at RVA 0x1A50B20):
//     +0x140  int32         highest zone ID in the file
//     +0x148  const char**  names, indexed by zoneId - 1 (unused IDs point at "")
//     +0x150  char*         string pool that owns the names (lives for the whole session)
//   ObjectZoneManager::GetZoneName (RVA 0x2378B0) is exactly names[id - 1], without a bounds check.
namespace ZoneList {
// True once the game has loaded zonelist.ids (from the first frames of boot onwards).
bool Ready();
// Highest zone ID in the game's table, 0 while not ready.
std::uint32_t MaxId();
// Name of a zone ID, or nullptr for an unknown/unused ID or while not ready. The pointer stays valid
// for the whole game session.
const char* Name(std::uint32_t zoneId);
// Reads the game's table again (after the framework appended names to it, v0.9.312).
void Refresh();
}
