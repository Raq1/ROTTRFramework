#pragma once
// Live link to the ROTTR Modding Suite: a local named pipe \\.\pipe\ROTTRModdingSuite carrying one JSON object per
// line. The Suite sends the values it edits (DTP values: object data, components, hair and cloth settings;
// material constants; which materials are shown) and the game shows them at once, without rebuilding the mod: DTP
// values as overrides of their own (DtpEdit, owner kSuiteOwner), material constants through the Material Editor's
// uploads, materials hidden as the outfit toggles hide them. A built outfit's copies of a resource (its renamed and
// copied IDs, CustomOutfits::ResourceCopies) get the edits of the resource the Suite edits.
namespace SuiteLink {
// Starts the pipe thread. Call once at startup (after DtpEdit::Initialize).
bool Initialize();
// Game thread, once per tick: runs the Suite's commands (material edits wait for their material's details).
void TickGameThread();
// Whether the Suite needs the Material Editor's capture running (its material list and uploads).
bool WantsMaterials();
}  // namespace SuiteLink
