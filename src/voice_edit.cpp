// An outfit's voice settings while it is worn (see voice_edit.h). Addresses: the retail Steam ROTTR.exe
// (GameBuild::kSupportedTimestamp); the hooked function is checked against its first bytes.
#include "voice_edit.h"

#include "dtp_edit.h"
#include "game_build.h"
#include "log.h"

#include <MinHook.h>
#include <Windows.h>

#include <atomic>
#include <cstring>
#include <map>

namespace VoiceEdit {
namespace {
// BarkComponent::PlayBark(this, const dtp::Bark*, ActionGraphNode*) -> PlayBarkResult (0 fail, 1 success, 2 queued)
// (TR2_x64_final 0x3CF510, found in ROTTR.exe through BarkComponent::PlayBark(int), which calls it)
constexpr std::uintptr_t kRvaPlayBark = 0x003AB990;
constexpr std::uint8_t kPlayBarkSig[] = {0x48, 0x89, 0x54, 0x24, 0x10, 0x48, 0x89, 0x4C, 0x24, 0x08, 0x55, 0x53, 0x56, 0x57, 0x41, 0x56,
                                         0x48, 0x8D, 0x6C, 0x24, 0xC9, 0x48, 0x81, 0xEC, 0xE0, 0x00, 0x00, 0x00, 0x4D, 0x8B, 0xF0, 0x48};
constexpr std::size_t kBarkSize = 0x28;   // dtp::Bark
constexpr int kOwner = DtpEdit::kSuiteOwner + 1;   // DtpEdit's owner of the worn outfit's values

using PlayBarkFn = int(__fastcall*)(void*, const std::uint8_t*, void*);
PlayBarkFn g_origPlayBark = nullptr;
bool g_installed = false;

// ---- the table the hook reads (immutable once published; old ones are kept: they are small) ----
struct Vocals {
    const std::uint8_t* barks = nullptr;   // the loaded dtp::Vocals' m_barks
    std::uint32_t count = 0;
    std::vector<float> chance;             // per bark (1: always)
    mutable std::vector<ULONGLONG> rolledAt;   // the last roll per bark and what it gave (the game thread only)
    mutable std::vector<char> rolled;
};
struct Table {
    std::vector<Vocals> vocals;
};
std::atomic<const Table*> g_table{nullptr};
std::uint32_t g_seed = 0x6D2B79F5u;

int __fastcall HookPlayBark(void* self, const std::uint8_t* bark, void* node) {
    if (const Table* table = g_table.load(std::memory_order_acquire); table && bark)
        for (const Vocals& v : table->vocals) {
            if (bark < v.barks || bark >= v.barks + std::size_t(v.count) * kBarkSize) continue;
            const std::size_t i = std::size_t(bark - v.barks) / kBarkSize;
            const float chance = v.chance[i];
            if (chance >= 1.0f) break;
            const ULONGLONG now = GetTickCount64();
            if (!v.rolledAt[i] || now - v.rolledAt[i] > 2000) {
                g_seed = g_seed * 1664525u + 1013904223u;
                v.rolled[i] = float((g_seed >> 8) & 0xFFFF) / 65536.0f < chance;
                v.rolledAt[i] = now;
            }
            if (!v.rolled[i]) return 0;   // (ePlayBarkResult_Fail: as a bark that may not play now)
            break;
        }
    return g_origPlayBark(self, bark, node);
}

// ---- the game thread's side ----
std::string g_wornKey;
Settings g_worn;
std::map<std::uint32_t, const void*> g_vocalsData;   // the chances' vocals: their data when the table was made
bool g_warned = false;

// dtp::Vocals: u32 m_numBarks, m_barks at +8 (no C++ objects here: structured exception handling)
bool ReadVocalsHead(const void* data, std::uint32_t& count, const std::uint8_t*& barks) {
    __try {
        count = *static_cast<const std::uint32_t*>(data);
        barks = *reinterpret_cast<const std::uint8_t* const*>(static_cast<const std::uint8_t*>(data) + 8);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    return true;
}

// dtp::SoundPlex::ChoiceList: u32 m_numSounds +0, u8 m_sequence +0x18 (0 weight, 1 weightNoRepeat, 2 shuffle, 3 cycle),
// u32 m_counter +0x1C (cycle: the next child; shuffle: the children played, a bit each; weightNoRepeat: the last one).
// Returns the counter it found when it put it back to 0 (-1: it was in range). (No C++ objects: SEH.)
long long KeepChoiceInRange(std::uint8_t* choice) {
    __try {
        const std::uint32_t count = *reinterpret_cast<const std::uint32_t*>(choice);
        const std::uint8_t mode = choice[0x18];
        auto* counter = reinterpret_cast<std::uint32_t*>(choice + 0x1C);
        const std::uint32_t was = *counter;
        if (!count) return -1;
        bool reset = false;
        if (mode == 3) reset = was >= count;
        else if (mode == 2 && count <= 32) {
            const std::uint32_t all = count == 32 ? 0xFFFFFFFFu : (1u << count) - 1u;
            reset = (was & all) == all;
        }
        if (!reset) return -1;
        *counter = 0;
        return (long long)was;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
}
int g_choiceLogs = 0;

void KeepChoicesInRange() {
    if (!DtpEdit::Available()) return;
    for (const Value& v : g_worn.values) {
        if (!v.choice) continue;
        auto* data = static_cast<std::uint8_t*>(DtpEdit::DataPointer(v.dtp));
        if (!data) continue;
        const long long was = KeepChoiceInRange(data + v.offset);
        if (was >= 0 && g_choiceLogs++ < 20)
            Log::Writef("VoiceEdit: DTP %u +0x%X: the choice's place (%lld) was past its sounds: back to the first", v.dtp,
                        v.offset, was);
    }
}

void Publish() {
    std::map<std::uint32_t, std::vector<const Chance*>> byVocals;
    for (const auto& c : g_worn.chances) byVocals[c.vocals].push_back(&c);
    auto* table = new Table();
    g_vocalsData.clear();
    for (const auto& [id, chances] : byVocals) {
        const void* data = DtpEdit::Available() ? DtpEdit::DataPointer(id) : nullptr;
        g_vocalsData[id] = data;
        if (!data) continue;
        Vocals v;
        if (!ReadVocalsHead(data, v.count, v.barks)) continue;
        if (!v.barks || !v.count || v.count > 100000) continue;
        v.chance.assign(v.count, 1.0f);
        v.rolledAt.assign(v.count, 0);
        v.rolled.assign(v.count, 1);
        for (const Chance* c : chances)
            if (c->bark < v.count) v.chance[c->bark] = c->chance < 0.0f ? 0.0f : c->chance;
        table->vocals.push_back(std::move(v));
    }
    g_table.store(table, std::memory_order_release);
}
}  // namespace

bool Install() {
    if (g_installed) return true;
    if (!GameBuild::VerifyBytes(kRvaPlayBark, kPlayBarkSig, sizeof(kPlayBarkSig))) {
        Log::Write("VoiceEdit: BarkComponent::PlayBark is not where this framework expects it: outfits' bark chances are off");
        return false;
    }
    if (MH_CreateHook(reinterpret_cast<void*>(GameBuild::Base() + kRvaPlayBark), reinterpret_cast<void*>(&HookPlayBark),
                      reinterpret_cast<void**>(&g_origPlayBark)) != MH_OK) {
        Log::Write("VoiceEdit: the bark hook could not be created");
        return false;
    }
    g_installed = true;
    // (the Modding Suite looks for this text in the .asi to tell a framework that applies outfits' voice settings)
    Log::Detail("VoiceEdit: outfits' voice settings (barks, chances, sound choices) are applied while they are worn");
    return true;
}

void SetWorn(const std::string& key, const Settings* settings) {
    const std::string wanted = settings && !settings->Empty() ? key : std::string();
    if (wanted == g_wornKey) return;
    if (DtpEdit::Available()) DtpEdit::ClearOwner(kOwner);   // (the game's values back)
    g_wornKey = wanted;
    g_worn = wanted.empty() ? Settings() : *settings;
    if (!wanted.empty()) {
        int applied = 0, pending = 0, refused = 0;
        if (!DtpEdit::Available()) {
            if (!g_warned) Log::Writef("VoiceEdit: %s: game data edits are unavailable (%s): its voice values are not set", key.c_str(), DtpEdit::Status());
            g_warned = true;
        } else {
            for (const Value& v : g_worn.values) {
                DtpEdit::Field field;
                std::string error;
                if (!DtpEdit::ParseField(v.dtp, v.offset, v.type, field, error)) {
                    ++refused;
                    continue;
                }
                switch (DtpEdit::SetOverride(kOwner, field, v.value, v.expect, v.size, error)) {
                    case DtpEdit::SetResult::Applied: ++applied; break;
                    case DtpEdit::SetResult::Pending: ++pending; break;
                    default:
                        ++refused;
                        Log::Detailf("VoiceEdit: %s: DTP %u +0x%X: %s", key.c_str(), v.dtp, v.offset, error.c_str());
                        break;
                }
            }
        }
        Log::Detailf("VoiceEdit: %s worn: %d voice value(s) set, %d waiting for their data, %d refused; %zu bark chance(s)",
                    key.c_str(), applied, pending, refused, g_worn.chances.size());
        KeepChoicesInRange();
    }
    if (g_installed) Publish();
}

void Tick() {
    if (!g_worn.values.empty()) KeepChoicesInRange();
    if (!g_installed || g_worn.chances.empty() || !DtpEdit::Available()) return;
    static ULONGLONG last = 0;
    const ULONGLONG now = GetTickCount64();
    if (now - last < 500) return;
    last = now;
    for (const auto& [id, data] : g_vocalsData)
        if (DtpEdit::DataPointer(id) != data) {   // (loaded, unloaded or loaded anew)
            Publish();
            return;
        }
}
}  // namespace VoiceEdit
