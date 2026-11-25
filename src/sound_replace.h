#pragma once
// Mods' sounds played instead of the game's (mod.json "waves", and an outfit's "voice": Lara's efforts, pain and death
// sounds while that outfit is worn).
//
// The game plays a bank sound (a "wave": sound resource type 6, its resource ID) by building a cdc::SoundPlexWave on
// its audio thread (cdc::SoundPlexWave::SoundPlexWave, from PlexFactory::CreatePlexHierarchy) with the wave's
// cdc::Sample, for a cdc::SoundOwner that records which object emits it (+0x344: its zone ID). The hook hands that
// constructor our own cdc::Sample instead (an FMOD sound made from the mod's WAV file), so the game's voice keeps its
// 3D placement, volume, bus, filters and lip sync. A sound listed for the worn outfit's object wins over one listed for
// every emitter. The samples are made on the game thread (FMOD::System::createSound from memory, the audio engine's
// lock held) and kept for the session.
//
// The sounds the player's object plays are written (each wave once) to ROTTRFramework\lara_sounds.log, so modders can
// find the waves to replace (the Modding Suite lists them).
//
// Lara's death screams, panic and breathing are sound streams (.mul files, cdc::MultiplexStream), not waves. An outfit's
// voice can list streams too: their files go into the framework's archive under names of our own
// (audio\streams\rmsvoice\...), and cdc::MultiplexStream::Init is handed such a name instead of the game's while the
// outfit is worn (the game's stream found by its name's hash, as the archives index it).
//
// An entry can be for one voice language ("language"): while the game's voice language is that one, it is played
// instead of the entry for every language of the same sound (none: the game's own); the table is made again when the
// voice language changes.
#include <cstdint>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

namespace SoundReplace {
struct Entry {
    std::uint32_t wave = 0;                       // the sound resource (wave) ID; a stream's name hash (stream)
    std::vector<std::filesystem::path> files;     // WAV files (one picked at random each time, not the same twice);
                                                  // a stream's: .mul files
    std::string label;                            // for the log
    bool stream = false;                          // a sound stream (an outfit's voice only)
    std::vector<std::string> names;               // a stream's: the stream names its files have in the framework
                                                  // archive (one per file)
    int language = -1;                            // the game's voice language it is for (language_t: 0 English,
                                                  // 3 Italian, 6 Japanese...; -1: every language without one of its own)
};
// mod.json "language": the game's names (english, french, german, italian, latamspanish, iberspanish, japanese,
// portuguese, polish, russian, dutch, korean, chinese, simplechinese, ksa_english, arabic) or the number; -1: none.
int LanguageFromName(const std::string& name);
const char* LanguageName(int language);
// The game's voice language now (localstr_get_voice_language: the options' voice language), -1: not known.
int VoiceLanguage();
// (v0.9.455) The game's text language now (localstr_get_language), -1: not known.
int TextLanguage();
// The archive path hash of a sound stream name (pcx64-w\audio\streams\<name>.mul).
std::uint32_t StreamHash(const std::string& name);

// An outfit's foley settings (mod.json "foley"): a sound the worn outfit's object plays made quieter / louder (volume:
// a factor, 0: not heard) and higher / lower (pitch: semitones), for that object only (the sound data is shared with
// other characters: it is not changed). Applied as the sound starts: its cdc::SoundPlexWave's own volume scale (+0x2C,
// which cdc::SoundPlexLeaf::Update multiplies with its parent's into +0x30, cdc::Voice::Update's volume) and its
// cdc::Voice's base frequency (+0x28C, set once by VoiceCollection::Create from the sample's).
struct Adjustment {
    std::uint32_t wave = 0;
    float volume = 1.0f, pitch = 0.0f;
};

// Every emitter (mod.json "waves"). Before Install.
void AddGlobal(const std::vector<Entry>& entries, const std::string& mod);
// Per object (zone ID): the worn outfits' voices (game thread; replaces what was set before when it differs).
void SetVoices(const std::map<std::uint32_t, std::vector<Entry>>& voices);
// Per object (zone ID): the worn outfits' foley settings (game thread; cheap when unchanged).
void SetAdjustments(const std::map<std::uint32_t, std::vector<Adjustment>>& adjustments);
bool Install();   // the hook (when there are sounds to play, or to log the player's)
void Tick();      // game thread: samples made, the table published, the player's sounds logged
}  // namespace SoundReplace
