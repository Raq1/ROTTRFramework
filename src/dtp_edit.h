#pragma once
#include <cstdint>
#include <string>
#include <vector>

// Game data edits (written for the ROTTR Foundation Engine editor's Inspector, whose link was removed in v1.0.0; now
// the Modding Suite's live edits and the framework's own DTP reads): component settings, action graph node settings,
// scene item properties. A value lives at a body offset of a DTP resource. Loaded DTPs are found by id in the game's
// own table (DTPDataSection::s_dtpDataInstances): their data is the resource body as the archives store it, with the
// pointers resolved, so the files' offsets are the offsets in memory.
//
// Values are set as overrides by an owner (the Suite's: kSuiteOwner; 0 and the positive numbers were the editor's and
// its scene builds'), the newest owner's value winning. An override is written at once when its DTP is
// loaded, again whenever the DTP loads anew (checked every 500 ms), and the game's own value is written back when the
// last owner lets it go. Before the first write the game's value must be one of the expected ones (the value in the
// files, the value of an installed mod): a DTP with another layout (another mod's) is never written.
namespace DtpEdit {
struct Field {
    std::uint32_t dtp = 0;
    std::uint32_t offset = 0;
    char kind = 0;              // 'f' float, 'i' signed integer, 'u' unsigned integer, 'b' bool
    std::uint8_t size = 0;      // bytes at offset: 1, 2, 4 or 8
    std::uint8_t bit = 0;       // a bitfield: its first bit and its bit count in the integer at offset
    std::uint8_t bits = 0;      // 0 = the whole value
};

// The ROTTR Modding Suite's live edits (suite_link.cpp): an owner of its own.
constexpr int kSuiteOwner = 0x40000000;

bool Initialize();
bool Available();
const char* Status();

// type: "f32" "f64" "i8" "u8" "i16" "u16" "i32" "u32" "i64" "u64" "bool", "E:<enum>" (a 32-bit enum),
// "bf:<base>:<first bit>:<bit count>" (a bitfield of an integer or a bool)
bool ParseField(std::uint32_t dtp, std::uint32_t offset, const std::string& type, Field& out, std::string& error);
bool Loaded(std::uint32_t dtp);
void* DataPointer(std::uint32_t dtp);               // the loaded DTP's data (its body), or null
bool Read(const Field& field, std::string& value);  // false: the DTP is not loaded (or not readable)
// The loaded DTP whose data holds address (dataOut: its data), 0 if none: a scan of the whole table (about a million
// ids), for the rare checks of a pointer the game keeps to some DTP's data (enemy_ai.cpp: the battle settings).
std::uint32_t FindContaining(const void* address, const void** dataOut);
// The DTP is still in use with that data (not unloaded, not loaded anew elsewhere). Cheap.
bool StillResident(std::uint32_t dtp, const void* data);

enum class SetResult { Applied, Pending, Refused };
// expect: the values the game may have before the first write (empty = no check); fileSize: the body size of the DTP
// in the files (0 = unknown): the game's DTP must have that size (DTPDataSection::GetSize), else it is another version
SetResult SetOverride(int owner, const Field& field, const std::string& value, const std::vector<std::string>& expect,
                      std::uint32_t fileSize, std::string& error);
void ClearOverride(int owner, const Field& field);
void ClearOwner(int owner);
bool OriginalValue(const Field& field, std::string& value);  // the game's own value under an override, when known
int OverrideCount(int owner);                                 // -1: all owners

void TickGameThread();
}  // namespace DtpEdit
