#pragma once
// An outfit's voice settings (mod.json "voice_settings", made by the Modding Suite's "Lara's voice" editor), applied
// while that outfit is worn:
// - values of the game's voice data: the vocals' barks (dtp::Bark: m_enabled, m_priority, m_timeOutIndividual,
//   m_timeOutGlobal) and their sound plexes (a ChoiceList's m_numSounds / m_sequence / weights, an Assignment's volume,
//   pitch, delays, variations, max instances, a Silence's time), written into the loaded DTPs by DtpEdit (an owner of
//   their own) while the outfit is worn and given back to the game's values when it is not. A value is written only
//   over the game's own (the files' value: "expect"), in a DTP of the files' size.
// - a chance per bark: BarkComponent::PlayBark (every bark goes through it: the state control's SCPropFunction_Bark,
//   the animations' BARK_Play, action graph barks, the bark queue) is hooked; a bark of the worn outfit's list plays
//   only that often (one roll per bark kept 2 s, so the queue's retry does not roll again).
#include <cstdint>
#include <string>
#include <vector>

namespace VoiceEdit {
struct Value {
    std::uint32_t dtp = 0, offset = 0;
    std::string type, value;
    std::vector<std::string> expect;
    std::uint32_t size = 0;
    bool choice = false;   // a ChoiceList's m_numSounds ("choice": true): its m_counter is kept in range (Tick)
};
struct Chance {
    std::uint32_t vocals = 0, bark = 0;   // the vocals DTP, the bark's index in it
    float chance = 1.0f;
};
struct Settings {
    std::vector<Value> values;
    std::vector<Chance> chances;
    bool Empty() const { return values.empty() && chances.empty(); }
};

bool Install();   // the PlayBark hook (before MH_EnableHook(MH_ALL_HOOKS))
// Game thread: the worn outfit's settings (key: which outfit; empty / null: none). Cheap when unchanged.
void SetWorn(const std::string& key, const Settings* settings);
// Game thread, every frame: the chances' vocals found in memory (they load and unload with levels); the narrowed
// choices' counters kept in range (a cycle's next child, a shuffle's played children: the game's ChoiceList code never
// wraps a cycle whose counter is past m_numSounds, and reads an index from uninitialised memory when a shuffle has every
// child marked played; both happen when m_numSounds is lowered under a counter of the old count).
void Tick();
}  // namespace VoiceEdit
