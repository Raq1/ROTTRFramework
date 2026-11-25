#include "outfit_menu_rows.h"

#include "game_build.h"
#include "log.h"

#include <Windows.h>
#include <MinHook.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <limits>
#include <vector>

namespace OutfitMenuRows {
namespace {
// UiMenuStateCampsiteSkills (UiMenuStateCampsiteGear's base at 0): +0x170 m_traitIndexLookups (u16[32]), +0x1B0
// m_currentNumberOfTraits (signed char), +0x1B1 m_maxNumberOfTraits (signed char), +0x1B6 its last field
// (m_bShowingInfoPopup), sizeof 0x1C0 (TR2_x64_final.pdb). UiMenuStateCampsiteGear: +0x1E8 m_indexOfCheckMark (signed
// char), +0x1EC m_frameCount.
constexpr std::ptrdiff_t kRowsByte = 0x1B0;
constexpr std::ptrdiff_t kConsideredByte = 0x1B1;
constexpr std::ptrdiff_t kCheckMark = 0x1E8;
constexpr std::ptrdiff_t kRows = 0x1B8;         // int (padding): the rows
constexpr std::ptrdiff_t kConsidered = 0x1BC;   // int (padding): m_maxNumberOfTraits' count
constexpr unsigned int kObjectRows = 32;
constexpr unsigned int kExtraRows = 2048;
std::uint16_t g_extra[kExtraRows] = {};         // rows 32 and up
bool g_ready = false;                            // verified, relays built, OnEnter hook created
bool g_applied = false;                          // the patches are in the game's code
void* g_preparedState = nullptr;                 // the Gear menu whose ints were set from its bytes

// ---- the Gear menu's own code (retail ROTTR.exe RVAs) ---------------------------------------------------------------
// Instructions rewritten in place: the counts and the check mark row as ints. Each replacement is as long as the
// original or shorter (then NOPs); the registers they read hold the same values zero-extended (checked in the game's
// code: edi/ebx/ebp/r14/rdi are 0 where they are stored or compared).
struct Rewrite {
    std::uintptr_t rva;
    std::uint8_t length;
    std::uint8_t original[8];
    std::uint8_t replacement[8];
};
constexpr std::uint8_t N = 0x90;
constexpr Rewrite kRewrites[] = {
    // m_currentNumberOfTraits (byte +0x1B0) -> the rows (int +0x1B8)
    {0x8D948D, 8, {0x44, 0x0F, 0xBE, 0x83, 0xB0, 0x01, 0x00, 0x00}, {0x44, 0x8B, 0x83, 0xB8, 0x01, 0x00, 0x00, N}},  // AddTheCheckmark
    {0x8DBB39, 7, {0x0F, 0xBE, 0x81, 0xB0, 0x01, 0x00, 0x00}, {0x8B, 0x81, 0xB8, 0x01, 0x00, 0x00, N}},              // CanSelectIndex
    {0x8E6E5C, 8, {0x48, 0x0F, 0xBE, 0x86, 0xB0, 0x01, 0x00, 0x00}, {0x48, 0x63, 0x86, 0xB8, 0x01, 0x00, 0x00, N}},  // DetermineWhatWeaponToUpgrade...
    {0x8EB821, 7, {0x0F, 0xBE, 0x97, 0xB0, 0x01, 0x00, 0x00}, {0x8B, 0x97, 0xB8, 0x01, 0x00, 0x00, N}},              // FindSelectionIndex
    {0x8EB8A1, 7, {0x0F, 0xBE, 0x97, 0xB0, 0x01, 0x00, 0x00}, {0x8B, 0x97, 0xB8, 0x01, 0x00, 0x00, N}},
    {0x8EB963, 7, {0x0F, 0xBE, 0x97, 0xB0, 0x01, 0x00, 0x00}, {0x8B, 0x97, 0xB8, 0x01, 0x00, 0x00, N}},
    {0x8FD024, 7, {0x40, 0x38, 0xBE, 0xB0, 0x01, 0x00, 0x00}, {0x39, 0xBE, 0xB8, 0x01, 0x00, 0x00, N}},              // InventoryComponentCallBackResponder (cmp, edi = 0)
    {0x8FD0FE, 7, {0x0F, 0xBE, 0x86, 0xB0, 0x01, 0x00, 0x00}, {0x8B, 0x86, 0xB8, 0x01, 0x00, 0x00, N}},
    {0x8FE6C5, 6, {0x38, 0x9F, 0xB0, 0x01, 0x00, 0x00}, {0x39, 0x9F, 0xB8, 0x01, 0x00, 0x00}},                        // MenuStateSetup (cmp, ebx = 0)
    {0x8FE72A, 7, {0x0F, 0xBE, 0x87, 0xB0, 0x01, 0x00, 0x00}, {0x8B, 0x87, 0xB8, 0x01, 0x00, 0x00, N}},
    {0x9008D8, 8, {0x66, 0x44, 0x89, 0xB6, 0xB0, 0x01, 0x00, 0x00}, {0x4C, 0x89, 0xB6, 0xB8, 0x01, 0x00, 0x00, N}},  // OnCategoryIndexSelected: both counts cleared (r14 = 0)
    {0x9009A5, 8, {0x48, 0x0F, 0xBE, 0x86, 0xB0, 0x01, 0x00, 0x00}, {0x48, 0x63, 0x86, 0xB8, 0x01, 0x00, 0x00, N}},
    {0x939303, 7, {0x66, 0x89, 0xBE, 0xB0, 0x01, 0x00, 0x00}, {0x48, 0x89, 0xBE, 0xB8, 0x01, 0x00, 0x00}},            // PopulateSkills: both counts cleared (rdi = 0)
    {0x93954C, 7, {0x0F, 0xBE, 0x86, 0xB0, 0x01, 0x00, 0x00}, {0x8B, 0x86, 0xB8, 0x01, 0x00, 0x00, N}},
    {0x939558, 8, {0x48, 0x0F, 0xBE, 0x86, 0xB0, 0x01, 0x00, 0x00}, {0x48, 0x63, 0x86, 0xB8, 0x01, 0x00, 0x00, N}},
    {0x9396BA, 7, {0x0F, 0xBE, 0x86, 0xB0, 0x01, 0x00, 0x00}, {0x8B, 0x86, 0xB8, 0x01, 0x00, 0x00, N}},
    {0x9396EA, 8, {0x48, 0x0F, 0xBE, 0x86, 0xB0, 0x01, 0x00, 0x00}, {0x48, 0x63, 0x86, 0xB8, 0x01, 0x00, 0x00, N}},
    {0x9397C6, 8, {0x44, 0x0F, 0xBE, 0x86, 0xB0, 0x01, 0x00, 0x00}, {0x44, 0x8B, 0x86, 0xB8, 0x01, 0x00, 0x00, N}},
    {0x939840, 8, {0x44, 0x0F, 0xBE, 0x86, 0xB0, 0x01, 0x00, 0x00}, {0x44, 0x8B, 0x86, 0xB8, 0x01, 0x00, 0x00, N}},
    {0x9399D9, 8, {0x48, 0x0F, 0xBE, 0x86, 0xB0, 0x01, 0x00, 0x00}, {0x48, 0x63, 0x86, 0xB8, 0x01, 0x00, 0x00, N}},
    {0x939B3F, 8, {0x48, 0x0F, 0xBE, 0x86, 0xB0, 0x01, 0x00, 0x00}, {0x48, 0x63, 0x86, 0xB8, 0x01, 0x00, 0x00, N}},
    {0x939BDC, 8, {0x44, 0x0F, 0xBE, 0x86, 0xB0, 0x01, 0x00, 0x00}, {0x44, 0x8B, 0x86, 0xB8, 0x01, 0x00, 0x00, N}},
    {0x939CDF, 7, {0x0F, 0xBE, 0x86, 0xB0, 0x01, 0x00, 0x00}, {0x8B, 0x86, 0xB8, 0x01, 0x00, 0x00, N}},
    // m_maxNumberOfTraits (byte +0x1B1) -> int +0x1BC
    {0x900945, 6, {0xFE, 0x86, 0xB1, 0x01, 0x00, 0x00}, {0xFF, 0x86, 0xBC, 0x01, 0x00, 0x00}},                        // inc
    {0x9393FE, 6, {0xFE, 0x86, 0xB1, 0x01, 0x00, 0x00}, {0xFF, 0x86, 0xBC, 0x01, 0x00, 0x00}},
    {0x939973, 6, {0xFE, 0x86, 0xB1, 0x01, 0x00, 0x00}, {0xFF, 0x86, 0xBC, 0x01, 0x00, 0x00}},
    {0x939C59, 8, {0x44, 0x0F, 0xBE, 0x86, 0xB1, 0x01, 0x00, 0x00}, {0x44, 0x8B, 0x86, 0xBC, 0x01, 0x00, 0x00, N}},
    {0x939DFA, 8, {0x44, 0x0F, 0xBE, 0x86, 0xB1, 0x01, 0x00, 0x00}, {0x44, 0x8B, 0x86, 0xBC, 0x01, 0x00, 0x00, N}},
    {0x939E51, 8, {0x44, 0x0F, 0xBE, 0x86, 0xB1, 0x01, 0x00, 0x00}, {0x44, 0x8B, 0x86, 0xBC, 0x01, 0x00, 0x00, N}},
    // m_indexOfCheckMark (byte +0x1E8) -> int +0x1E8 (with the padding up to m_frameCount)
    {0x8D9409, 6, {0x88, 0x83, 0xE8, 0x01, 0x00, 0x00}, {0x89, 0x83, 0xE8, 0x01, 0x00, 0x00}},                        // AddTheCheckmark (eax: FindSelectionIndex's int)
    {0x8D94D5, 6, {0x88, 0x93, 0xE8, 0x01, 0x00, 0x00}, {0x89, 0x93, 0xE8, 0x01, 0x00, 0x00}},                        // (edx: the row)
    {0x8D94DD, 7, {0x40, 0x88, 0xAB, 0xE8, 0x01, 0x00, 0x00}, {0x89, 0xAB, 0xE8, 0x01, 0x00, 0x00, N}},              // (ebp = 0)
    {0x8D9510, 7, {0x0F, 0xBE, 0x83, 0xE8, 0x01, 0x00, 0x00}, {0x8B, 0x83, 0xE8, 0x01, 0x00, 0x00, N}},
    {0x8FE9CC, 6, {0x88, 0x87, 0xE8, 0x01, 0x00, 0x00}, {0x89, 0x87, 0xE8, 0x01, 0x00, 0x00}},                        // MenuStateSetup (FindSelectionIndex's int)
    {0x8FE9D7, 4, {0x48, 0x0F, 0xBE, 0xC0}, {0x48, 0x63, 0xC0, N}},                                                    // movsx rax, al -> movsxd rax, eax
    {0x900818, 8, {0x48, 0x0F, 0xBE, 0x86, 0xE8, 0x01, 0x00, 0x00}, {0x48, 0x63, 0x86, 0xE8, 0x01, 0x00, 0x00, N}},  // OnCategoryIndexSelected
    {0x91A983, 7, {0x0F, 0xBE, 0x8F, 0xE8, 0x01, 0x00, 0x00}, {0x8B, 0x8F, 0xE8, 0x01, 0x00, 0x00, N}},              // OnInputActionPressed
    {0x956283, 7, {0x0F, 0xBE, 0x83, 0xE8, 0x01, 0x00, 0x00}, {0x8B, 0x83, 0xE8, 0x01, 0x00, 0x00, N}},              // UpdateButtonLegend
    {0x956307, 7, {0x0F, 0xBE, 0x83, 0xE8, 0x01, 0x00, 0x00}, {0x8B, 0x83, 0xE8, 0x01, 0x00, 0x00, N}},
    {0x95633E, 7, {0x0F, 0xBE, 0x83, 0xE8, 0x01, 0x00, 0x00}, {0x8B, 0x83, 0xE8, 0x01, 0x00, 0x00, N}},
    {0x95B465, 8, {0x49, 0x0F, 0xBE, 0x86, 0xE8, 0x01, 0x00, 0x00}, {0x49, 0x63, 0x86, 0xE8, 0x01, 0x00, 0x00, N}},  // UpdateInformationWidgetAndButtonLegend
};

// The Gear menu's row writers: "mov word [rsi + rax*2 + 0x170], reg16" then "inc byte [rsi + 0x1B0]" (rax: the row,
// from the rows int by the rewrite before it). Replaced by a jump to a relay that writes the row to the object (rows
// below 32) or the table (32 and up) and counts it in the rows int; past the table's rows nothing is written or
// counted (the list ends there).
struct Writer {
    std::uintptr_t rva;
    std::uint8_t writeLength;   // the mov's (9 with a REX prefix, else 8); the inc follows
};
constexpr Writer kWriters[] = {
    {0x939566, 9},   // PopulateSkills: the outfits (r15w)
    {0x9396F2, 8},   // PopulateSkills: the broken outfits' parts (di)
    {0x9399E5, 9},   // PopulateSkills: the gear (r15w)
    {0x939B47, 8},   // PopulateSkills: the broken gear's parts (bx)
    {0x9009AD, 8},   // OnCategoryIndexSelected (bp)
};
constexpr std::uint8_t kRowsByteIncrement[] = {0xFE, 0x86, 0xB0, 0x01, 0x00, 0x00};   // inc byte [rsi + 0x1B0]

// Every reader of the rows in the campsite menus' code (UiMenuStateCampsiteSkills and the menus deriving from it):
// "movzx r32, word/byte [base + idx*2 + 0x170]". Rows 32 and up (only the Gear menu has them) read the table.
constexpr std::uintptr_t kReadSites[] = {
    0x008D94B2ull, 0x008E0165ull, 0x008E023Full,
    0x008E6EEFull, 0x008E6F1Aull, 0x008E8B00ull, 0x008E8FB4ull, 0x008E921Full,
    0x008EB852ull, 0x008EB8C2ull, 0x008EB992ull,
    0x008FC8AAull, 0x008FE6DEull, 0x008FE9DBull,
    0x009006E4ull, 0x00900820ull, 0x0091ACCFull,
    0x00925331ull,
    0x0092F82Dull, 0x0092F9DAull, 0x0092FBE4ull,
    0x00930056ull, 0x009301A3ull, 0x0093032Full, 0x00930A96ull,
    0x00938DEFull, 0x00938F63ull, 0x00939000ull, 0x009390B0ull,
    0x0093A7A4ull, 0x0093A7B9ull, 0x0093C991ull, 0x0093C9A6ull,
    0x00942967ull, 0x00942BE2ull, 0x00942D23ull, 0x00942E0Eull, 0x00942F9Aull,
    0x00955E08ull, 0x009561FFull, 0x00956457ull, 0x0095646Bull, 0x00956624ull,
    0x0095A724ull, 0x0095AFB1ull, 0x0095B056ull,
    0x0095B46Dull, 0x0095B47Aull, 0x0095B49Full,
    0x0095B663ull, 0x0095B67Eull, 0x0095BAF7ull, 0x0095BDD1ull, 0x0095C2B6ull,
};

// UiMenuStateCampsiteGear::OnEnter (its vtable's slot 3).
constexpr std::uintptr_t kRvaGearOnEnter = 0x907730;
constexpr std::uint8_t kGearOnEnterSig[] = {0x48, 0x8B, 0xC4, 0x53, 0x41, 0x56, 0x48, 0x81, 0xEC,
                                            0x98, 0x00, 0x00, 0x00, 0x48, 0x89, 0x68, 0x10};

// ---- code building (pure: tested off the game) -----------------------------------------------------------------------
struct ReadSite {
    std::uintptr_t rva = 0;
    std::array<std::uint8_t, 9> original{};
    std::size_t length = 0;
    unsigned int dest = 0;
    unsigned int index = 0;
};

bool DecodeRead(const std::uint8_t* p, std::uintptr_t rva, ReadSite& out) {
    std::size_t i = 0;
    std::uint8_t rex = 0;
    if ((p[i] & 0xF0u) == 0x40u) rex = p[i++];
    if (p[i++] != 0x0F) return false;
    const std::uint8_t op = p[i++];
    if (op != 0xB6 && op != 0xB7) return false;
    const std::uint8_t modrm = p[i++];
    if ((modrm & 0xC7u) != 0x84u) return false;   // mod 10, r/m: SIB
    const std::uint8_t sib = p[i++];
    if ((sib >> 6) != 1u) return false;            // scale 2
    std::int32_t displacement = 0;
    std::memcpy(&displacement, p + i, sizeof(displacement));
    i += sizeof(displacement);
    if (displacement != 0x170 || i < 8 || i > out.original.size()) return false;
    out.rva = rva;
    out.length = i;
    out.dest = ((modrm >> 3) & 7u) | ((rex & 0x04u) ? 8u : 0u);
    out.index = ((sib >> 3) & 7u) | ((rex & 0x02u) ? 8u : 0u);
    std::memcpy(out.original.data(), p, i);
    return true;
}

bool Rel32(std::uint8_t* at, std::uintptr_t from, std::uintptr_t to) {   // at: 4 bytes; from: the next instruction
    const std::intptr_t delta = static_cast<std::intptr_t>(to) - static_cast<std::intptr_t>(from);
    if (delta < std::numeric_limits<std::int32_t>::min() || delta > std::numeric_limits<std::int32_t>::max()) return false;
    const std::int32_t rel = static_cast<std::int32_t>(delta);
    std::memcpy(at, &rel, sizeof(rel));
    return true;
}

// A read's relay at stub (its address in the game's process: stubAddress): rows below 32 run the original read; rows
// 32 and up read table[row - 32]. The flags are kept (the reads change none).
std::size_t BuildReadRelay(const ReadSite& site, std::uintptr_t siteAddress, std::uint8_t* stub, std::uintptr_t stubAddress,
                           std::uintptr_t table) {
    std::size_t n = 0;
    auto put = [&](std::uint8_t b) { stub[n++] = b; };
    const std::uint8_t op = site.original[((site.original[0] & 0xF0u) == 0x40u) ? 2 : 1];   // 0xB6 / 0xB7
    put(0x9C);                                                                            // pushfq
    put(static_cast<std::uint8_t>(0x48u | (site.index >= 8 ? 1u : 0u)));                  // cmp idx, 0x20
    put(0x83);
    put(static_cast<std::uint8_t>(0xF8u | (site.index & 7u)));
    put(0x20);
    put(0x7C);                                                                            // jl own
    const std::size_t toOwn = n++;
    const bool scratch = site.dest == site.index;
    const unsigned int base = scratch ? (site.index == 1u ? 2u : 1u) : site.dest;         // rcx / rdx when needed
    if (scratch) put(static_cast<std::uint8_t>(0x50u | base));                            // push base
    put(static_cast<std::uint8_t>(0x48u | (base >= 8 ? 1u : 0u)));                        // mov base, table - 0x40
    put(static_cast<std::uint8_t>(0xB8u | (base & 7u)));
    const std::uintptr_t biased = table - 0x40u;
    std::memcpy(stub + n, &biased, sizeof(biased));
    n += sizeof(biased);
    const std::uint8_t rex = static_cast<std::uint8_t>(0x40u | (site.dest >= 8 ? 4u : 0u) | (site.index >= 8 ? 2u : 0u) |
                                                       (base >= 8 ? 1u : 0u));
    if (rex != 0x40u) put(rex);
    put(0x0F);                                                                            // movzx dest, [base + idx*2]
    put(op);
    put(static_cast<std::uint8_t>(0x44u | ((site.dest & 7u) << 3)));
    put(static_cast<std::uint8_t>(0x40u | ((site.index & 7u) << 3) | (base & 7u)));
    put(0x00);
    if (scratch) put(static_cast<std::uint8_t>(0x58u | base));                            // pop base
    put(0x9D);                                                                            // popfq
    put(0xE9);                                                                            // jmp back
    if (!Rel32(stub + n, stubAddress + n + 4, siteAddress + site.length)) return 0;
    n += 4;
    stub[toOwn] = static_cast<std::uint8_t>(n - (toOwn + 1));
    std::memcpy(stub + n, site.original.data(), site.length);                              // own: the original read
    n += site.length;
    put(0x9D);                                                                            // popfq
    put(0xE9);
    if (!Rel32(stub + n, stubAddress + n + 4, siteAddress + site.length)) return 0;
    n += 4;
    return n;
}

// A writer's relay: write is the game's "mov word [rsi + rax*2 + 0x170], reg16" (8 or 9 bytes); back: the instruction
// after the writer's inc.
std::size_t BuildWriterRelay(const std::uint8_t* write, std::size_t writeLength, std::uintptr_t back, std::uint8_t* stub,
                             std::uintptr_t stubAddress, std::uintptr_t table, unsigned int tableRows) {
    // the source register: 66 [REX] 89 modrm(10 reg 100) sib(01 000 110: rax*2 + rsi) disp32
    std::size_t i = 0;
    if (write[i++] != 0x66) return 0;
    std::uint8_t rex = 0;
    if ((write[i] & 0xF0u) == 0x40u) rex = write[i++];
    if (write[i++] != 0x89) return 0;
    const std::uint8_t modrm = write[i++];
    if ((modrm & 0xC7u) != 0x84u || write[i++] != 0x46) return 0;
    std::int32_t displacement = 0;
    std::memcpy(&displacement, write + i, sizeof(displacement));
    i += sizeof(displacement);
    if (displacement != 0x170 || i != writeLength || (rex && rex != 0x44)) return 0;   // (REX.R only: r8w..r15w)
    const unsigned int reg = ((modrm >> 3) & 7u) | ((rex & 0x04u) ? 8u : 0u);
    if (reg == 0 || reg == 2 || reg == 6) return 0;   // (rax, rdx, rsi: the row, the scratch, the menu)

    std::size_t n = 0;
    auto put = [&](std::uint8_t b) { stub[n++] = b; };
    put(0x48); put(0x83); put(0xF8); put(0x20);                       // cmp rax, 0x20
    put(0x73);                                                        // jae table
    const std::size_t toTable = n++;
    std::memcpy(stub + n, write, writeLength);                         // the game's write (rows below 32)
    n += writeLength;
    put(0xEB);                                                        // jmp count
    const std::size_t toCount1 = n++;
    stub[toTable] = static_cast<std::uint8_t>(n - (toTable + 1));
    put(0x48); put(0x3D);                                             // cmp rax, 0x20 + rows
    const std::uint32_t limit = kObjectRows + tableRows;
    std::memcpy(stub + n, &limit, sizeof(limit));
    n += sizeof(limit);
    put(0x73);                                                        // jae done (past the table: not written, not counted)
    const std::size_t toDone = n++;
    put(0x52);                                                        // push rdx
    put(0x48); put(0xBA);                                             // mov rdx, table - 0x40
    const std::uintptr_t biased = table - 0x40u;
    std::memcpy(stub + n, &biased, sizeof(biased));
    n += sizeof(biased);
    put(0x66);                                                        // mov word [rdx + rax*2], reg16
    if (reg >= 8) put(0x44);
    put(0x89);
    put(static_cast<std::uint8_t>(0x04u | ((reg & 7u) << 3)));
    put(0x42);
    put(0x5A);                                                        // pop rdx
    stub[toCount1] = static_cast<std::uint8_t>(n - (toCount1 + 1));
    put(0xFF); put(0x86);                                             // inc dword [rsi + 0x1B8]
    const std::int32_t rows = static_cast<std::int32_t>(kRows);
    std::memcpy(stub + n, &rows, sizeof(rows));
    n += sizeof(rows);
    stub[toDone] = static_cast<std::uint8_t>(n - (toDone + 1));
    put(0xE9);                                                        // jmp back
    if (!Rel32(stub + n, stubAddress + n + 4, back)) return 0;
    n += 4;
    return n;
}

// ---- in the game ------------------------------------------------------------------------------------------------------
void* AllocateNear(std::size_t size) {   // within rel32 reach of the game's code
    const std::uintptr_t base = GameBuild::Base();
    SYSTEM_INFO si{};
    GetSystemInfo(&si);
    const std::uintptr_t gran = std::max<std::uintptr_t>(si.dwAllocationGranularity, 0x10000u);
    auto up = [gran](std::uintptr_t v) { return (v + gran - 1u) & ~(gran - 1u); };
    constexpr std::uintptr_t kSpan = 0x60000000ull;
    for (std::uintptr_t cursor = up(base + 0x04000000ull); cursor < base + kSpan;) {
        MEMORY_BASIC_INFORMATION mbi{};
        if (!VirtualQuery(reinterpret_cast<const void*>(cursor), &mbi, sizeof(mbi))) break;
        const std::uintptr_t regionBase = reinterpret_cast<std::uintptr_t>(mbi.BaseAddress);
        const std::uintptr_t regionEnd = regionBase + mbi.RegionSize;
        if (mbi.State == MEM_FREE) {
            const std::uintptr_t candidate = up(std::max(cursor, regionBase));
            if (candidate < regionEnd && regionEnd - candidate >= size)
                if (void* p = VirtualAlloc(reinterpret_cast<void*>(candidate), size, MEM_RESERVE | MEM_COMMIT,
                                           PAGE_EXECUTE_READWRITE))
                    return p;
        }
        if (regionEnd <= cursor) break;
        cursor = up(regionEnd);
    }
    std::uintptr_t best = 0;
    for (std::uintptr_t cursor = base > kSpan ? base - kSpan : 0x10000ull; cursor < base;) {
        MEMORY_BASIC_INFORMATION mbi{};
        if (!VirtualQuery(reinterpret_cast<const void*>(cursor), &mbi, sizeof(mbi))) break;
        const std::uintptr_t regionBase = reinterpret_cast<std::uintptr_t>(mbi.BaseAddress);
        const std::uintptr_t regionEnd = regionBase + mbi.RegionSize;
        if (mbi.State == MEM_FREE) {
            const std::uintptr_t candidate = up(regionBase);
            if (candidate < regionEnd && regionEnd - candidate >= size) best = candidate;
        }
        if (regionEnd <= cursor) break;
        cursor = up(regionEnd);
    }
    return best ? VirtualAlloc(reinterpret_cast<void*>(best), size, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE) : nullptr;
}

bool WriteCode(std::uintptr_t rva, const std::uint8_t* bytes, std::size_t size) {
    auto* at = reinterpret_cast<std::uint8_t*>(GameBuild::Base() + rva);
    DWORD old = 0;
    if (!VirtualProtect(at, size, PAGE_EXECUTE_READWRITE, &old)) return false;
    std::memcpy(at, bytes, size);
    FlushInstructionCache(GetCurrentProcess(), at, size);
    DWORD ignored = 0;
    VirtualProtect(at, size, old, &ignored);
    return true;
}

struct Patch {
    std::uintptr_t rva = 0;
    std::vector<std::uint8_t> original, replacement;
};
std::vector<Patch> g_patches;   // built by Install, written by the first OnEnter

bool Apply() {
    std::size_t written = 0;
    for (; written < g_patches.size(); ++written) {
        const Patch& p = g_patches[written];
        if (!GameBuild::VerifyBytes(p.rva, p.original.data(), p.original.size()) ||
            !WriteCode(p.rva, p.replacement.data(), p.replacement.size()))
            break;
    }
    if (written == g_patches.size()) return true;
    for (std::size_t k = 0; k < written; ++k) WriteCode(g_patches[k].rva, g_patches[k].original.data(), g_patches[k].original.size());
    Log::Writef("OutfitMenuRows: the campsite's outfit list could not be changed (at RVA 0x%llX); it stays the game's",
                static_cast<unsigned long long>(g_patches[written].rva));
    return false;
}

using OnEnterFn = std::uintptr_t(__fastcall*)(void*, void*, void*);
OnEnterFn g_origOnEnter = nullptr;

std::uintptr_t __fastcall HookGearOnEnter(void* state, void* from, void* to) {
    if (g_ready && !g_applied) {
        g_applied = Apply();
        if (g_applied)
            Log::Detailf("OutfitMenuRows: the campsite's outfit list holds up to %u rows (%zu code changes)",
                         kObjectRows + kExtraRows, g_patches.size());
        else
            g_ready = false;
    }
    if (g_applied && state && state != g_preparedState) {
        // the ints from the game's bytes (the Gear menu is made once, when the game starts: before the patches)
        auto* p = static_cast<std::uint8_t*>(state);
        const std::int32_t rows = static_cast<std::int8_t>(p[kRowsByte]);
        const std::int32_t considered = static_cast<std::int8_t>(p[kConsideredByte]);
        const std::int32_t check = static_cast<std::int8_t>(p[kCheckMark]);
        std::memcpy(p + kRows, &rows, sizeof(rows));
        std::memcpy(p + kConsidered, &considered, sizeof(considered));
        std::memcpy(p + kCheckMark, &check, sizeof(check));
        g_preparedState = state;
    }
    return g_origOnEnter(state, from, to);
}
}  // namespace

bool Install() {
    if (g_ready) return true;
    const std::uintptr_t base = GameBuild::Base();
    if (!base) return false;
    auto fail = [](const char* what, std::uintptr_t rva) {
        Log::Writef("OutfitMenuRows: %s at RVA 0x%llX is not the game code this framework knows; the campsite's outfit "
                    "list stays the game's (32 rows)", what, static_cast<unsigned long long>(rva));
        return false;
    };
    if (!GameBuild::VerifyBytes(kRvaGearOnEnter, kGearOnEnterSig, sizeof(kGearOnEnterSig)))
        return fail("UiMenuStateCampsiteGear::OnEnter", kRvaGearOnEnter);
    for (const Rewrite& r : kRewrites)
        if (!GameBuild::VerifyBytes(r.rva, r.original, r.length)) return fail("a row count", r.rva);
    for (const Writer& w : kWriters) {
        const auto* at = reinterpret_cast<const std::uint8_t*>(base + w.rva);
        if (std::memcmp(at + w.writeLength, kRowsByteIncrement, sizeof(kRowsByteIncrement)) != 0)
            return fail("a row writer", w.rva);
    }
    std::vector<ReadSite> reads;
    for (const std::uintptr_t rva : kReadSites) {
        ReadSite site;
        if (!DecodeRead(reinterpret_cast<const std::uint8_t*>(base + rva), rva, site)) return fail("a row reader", rva);
        reads.push_back(site);
    }

    constexpr std::size_t kBlock = 0x10000;
    auto* block = static_cast<std::uint8_t*>(AllocateNear(kBlock));
    if (!block) {
        Log::Write("OutfitMenuRows: no memory near the game's code for the relays; the campsite's outfit list stays the game's");
        return false;
    }
    const auto blockAddress = reinterpret_cast<std::uintptr_t>(block);
    const auto table = reinterpret_cast<std::uintptr_t>(&g_extra[0]);
    std::size_t used = 0;
    std::vector<Patch> patches;
    auto jumpPatch = [&](std::uintptr_t rva, std::size_t length, std::uintptr_t stub) -> bool {
        Patch p;
        p.rva = rva;
        p.original.assign(reinterpret_cast<const std::uint8_t*>(base + rva), reinterpret_cast<const std::uint8_t*>(base + rva) + length);
        p.replacement.assign(length, 0x90);
        p.replacement[0] = 0xE9;
        if (!Rel32(p.replacement.data() + 1, base + rva + 5, stub)) return false;
        patches.push_back(std::move(p));
        return true;
    };
    bool built = true;
    for (const Writer& w : kWriters) {
        used = (used + 15u) & ~std::size_t(15);
        const auto* at = reinterpret_cast<const std::uint8_t*>(base + w.rva);
        const std::size_t length = w.writeLength + sizeof(kRowsByteIncrement);
        const std::size_t size = BuildWriterRelay(at, w.writeLength, base + w.rva + length, block + used, blockAddress + used,
                                                  table, kExtraRows);
        if (!size || !jumpPatch(w.rva, length, blockAddress + used)) { built = false; break; }
        used += size;
    }
    for (const ReadSite& site : reads) {
        if (!built) break;
        used = (used + 15u) & ~std::size_t(15);
        if (used + 64 > kBlock) { built = false; break; }
        const std::size_t size = BuildReadRelay(site, base + site.rva, block + used, blockAddress + used, table);
        if (!size || !jumpPatch(site.rva, site.length, blockAddress + used)) { built = false; break; }
        used += size;
    }
    if (!built) {
        VirtualFree(block, 0, MEM_RELEASE);
        Log::Write("OutfitMenuRows: the relays could not be built; the campsite's outfit list stays the game's");
        return false;
    }
    FlushInstructionCache(GetCurrentProcess(), block, used);
    for (const Rewrite& r : kRewrites) {
        Patch p;
        p.rva = r.rva;
        p.original.assign(r.original, r.original + r.length);
        p.replacement.assign(r.replacement, r.replacement + r.length);
        patches.push_back(std::move(p));
    }
    if (MH_CreateHook(reinterpret_cast<void*>(base + kRvaGearOnEnter), reinterpret_cast<void*>(&HookGearOnEnter),
                      reinterpret_cast<void**>(&g_origOnEnter)) != MH_OK) {
        VirtualFree(block, 0, MEM_RELEASE);
        Log::Write("OutfitMenuRows: the UiMenuStateCampsiteGear::OnEnter hook could not be created; the campsite's outfit "
                   "list stays the game's");
        return false;
    }
    g_patches = std::move(patches);
    g_ready = true;
    Log::Detailf("OutfitMenuRows: ready (%zu writers, %zu readers, %zu counts; put in place when the Outfits menu opens)",
                 std::size(kWriters), reads.size(), std::size(kRewrites));
    return true;
}

unsigned int Capacity() {
    return g_ready ? kObjectRows + kExtraRows : kObjectRows;
}
}  // namespace OutfitMenuRows
