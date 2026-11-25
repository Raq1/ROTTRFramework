// Mods' sounds played instead of the game's (see sound_replace.h). Addresses: the retail Steam ROTTR.exe
// (GameBuild::kSupportedTimestamp); every hooked or called function is checked against its first bytes.
#include "sound_replace.h"

#include "features.h"
#include "game_archive.h"
#include "game_build.h"
#include "log.h"
#include "zone_list.h"

#include <MinHook.h>
#include <Windows.h>

#include <algorithm>
#include <cstdint>
#include <array>
#include <atomic>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <mutex>
#include <memory>
#include <set>
#include <thread>
#include <unordered_map>

namespace SoundReplace {
namespace fs = std::filesystem;
namespace {
// cdc::SoundPlexWave::SoundPlexWave(this, PlexBucket*, const dtp::SoundPlex::Wave*, cdc::Sample*, const Controls*,
// const Controls3d*, SoundOwner*, uint32 plex DTP ID, SoundEffectOwner* buss, SoundPlex* parent)
constexpr std::uintptr_t kRvaWaveCtor = 0x00397070;
constexpr std::uint8_t kWaveCtorSig[] = {0x4C, 0x8B, 0xDC, 0x49, 0x89, 0x5B, 0x08, 0x49, 0x89, 0x6B, 0x10, 0x49, 0x89, 0x73, 0x18, 0x49,
                                         0x89, 0x7B, 0x20, 0x41, 0x56, 0x48, 0x83, 0xEC, 0x40, 0x48, 0x8B, 0x84, 0x24, 0x98, 0x00, 0x00};
constexpr std::uintptr_t kRvaWaveCtorCall = 0x0039F663;   // its one caller (CreatePlexHierarchy): call rel32 -> the ctor
constexpr std::uint8_t kWaveCtorCallSig[] = {0xE8, 0x08, 0x7A, 0xFF, 0xFF};
// cdc::Sample::Create(Type, int lengthInBytes, int loopStart, int loopEnd, int sampleRate, float highestPeak): the
// game heap's 0x38-byte sample, reference count 1
constexpr std::uintptr_t kRvaSampleCreate = 0x0039DCA0;
constexpr std::uint8_t kSampleCreateSig[] = {0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x6C, 0x24, 0x10, 0x48, 0x89, 0x74, 0x24, 0x18, 0x48,
                                             0x89, 0x7C, 0x24, 0x20, 0x41, 0x56, 0x48, 0x83, 0xEC, 0x20, 0x8B, 0x74, 0x24, 0x50, 0x8B, 0xD9};
// FMOD Ex (linked into the game): FMOD::System::createSound, FMOD::Sound::getDefaults
constexpr std::uintptr_t kRvaCreateSound = 0x00A5A2C0;
constexpr std::uint8_t kCreateSoundSig[] = {0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x74, 0x24, 0x10, 0x57, 0x48, 0x83, 0xEC, 0x40, 0x48,
                                            0x8B, 0xF2, 0x49, 0x8B, 0xD9, 0x48, 0x8D, 0x54, 0x24, 0x30, 0x41, 0x8B, 0xF8, 0xE8, 0x3E, 0x28};
constexpr std::uintptr_t kRvaGetDefaults = 0x00A5ABD0;
constexpr std::uint8_t kGetDefaultsSig[] = {0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x74, 0x24, 0x10, 0x57, 0x48, 0x83, 0xEC, 0x40, 0x48,
                                            0x8B, 0xF2, 0x49, 0x8B, 0xD9, 0x48, 0x8D, 0x54, 0x24, 0x30, 0x49, 0x8B, 0xF8, 0xE8, 0x6E, 0x6A};
// localstr_get_voice_language(): mov eax, [rip + disp32]; ret (TR2_x64_final 0x95DD0: g_languageOverride + 0x5C)
constexpr std::uintptr_t kRvaGetVoiceLanguage = 0x00083720;
constexpr std::uint8_t kGetVoiceLanguageSig[] = {0x8B, 0x05};
// localstr_get_language(): the same, the text language (TR2_x64_final 0x95A50: g_languageOverride + 4; retail 0xFB3AE8,
// the voice language's 0xFB3AF0: both written by the language change, retail 0x826C0 / TR2 0x94EF0)
constexpr std::uintptr_t kRvaGetTextLanguage = 0x000833A0;

// (v0.9.455) Bank sounds in the voice language. A .drm's sections (cdc::ResolveReceiver::ReceiveData, retail 0x347A99 /
// TR2 0x36AA16: for each 0x14-byte section header, +4 type, +0x10 locale mask) are loaded only when (locale & mask) ==
// mask, the mask being the file system's specialisation mask: Specialisation::BlockingChange(text, voice) (TR2 0x363A40)
// makes it the TEXT language's (s_targetMask); the voice language's (s_audioMask) is used for streams only
// (SNDAPP_StreamOpenFile / StreamRequestData). So bank sounds with a copy per language (Lara's efforts: one most
// languages share, English among them, and Italian, Japanese, Polish and Russian of their own) were the text
// language's: Lara grunted in Italian with English voices and Italian text. Now the section check's first two
// instructions (mov ecx, [rdx + 0xB]; mov eax, ecx: rdx is the header + 5) call kBankLocaleStub: for a sound bank
// section (type 6) whose locale has the voice language's bit, the text language's bit is added; for one without it, it
// is taken out; then eax = ecx as before. Every other section, and language-free sounds, as the game reads them.
constexpr std::uintptr_t kRvaSectionLocaleCheck = 0x00347A99;
constexpr std::uint8_t kSectionLocaleCheckSig[] = {0x8B, 0x4A, 0x0B, 0x8B, 0xC1, 0x23, 0x44, 0x24, 0x74,
                                                   0x44, 0x23, 0xF9, 0x3B, 0x44, 0x24, 0x74, 0x75};
constexpr std::uint8_t kSectionLocaleCheckBefore[] = {0x48, 0x83, 0xC2, 0x05};   // add rdx, 5 (at -0x1B)
// mov ecx, [rdx + 0xB]; cmp byte [rdx - 1], 6; jne out; push rbx; push rdx; mov rax, &text; mov edx, [rax];
// mov rax, &voice; mov ebx, [rax]; cmp ebx, edx; je done; (ebx, edx <= 15: language bits only) xor eax, eax;
// bts eax, ebx; test ecx, eax; jz clear; xor eax, eax; bts eax, edx; or ecx, eax; jmp done; clear: xor eax, eax;
// bts eax, edx; not eax; and ecx, eax; done: pop rdx; pop rbx; out: mov eax, ecx; ret
constexpr std::uint8_t kBankLocaleStub[] = {
    0x8B, 0x4A, 0x0B, 0x80, 0x7A, 0xFF, 0x06, 0x75, 0x45, 0x53, 0x52, 0x48, 0xB8, 0x88, 0x77, 0x66, 0x55, 0x44, 0x33,
    0x22, 0x11, 0x8B, 0x10, 0x48, 0xB8, 0x00, 0xFF, 0xEE, 0xDD, 0xCC, 0xBB, 0xAA, 0x99, 0x8B, 0x18, 0x39, 0xD3, 0x74,
    0x25, 0x83, 0xFB, 0x0F, 0x77, 0x20, 0x83, 0xFA, 0x0F, 0x77, 0x1B, 0x31, 0xC0, 0x0F, 0xAB, 0xD8, 0x85, 0xC1, 0x74,
    0x09, 0x31, 0xC0, 0x0F, 0xAB, 0xD0, 0x09, 0xC1, 0xEB, 0x09, 0x31, 0xC0, 0x0F, 0xAB, 0xD0, 0xF7, 0xD0, 0x21, 0xC1,
    0x5A, 0x5B, 0x89, 0xC8, 0xC3};
constexpr std::size_t kBankStubTextAt = 13, kBankStubVoiceAt = 25;   // the two imm64 addresses in it
// the foley settings' places (checked: the code that uses them): SoundPlexWave's m_voice (+0x40, stored after
// VoiceCollection::Create), SoundPlexLeaf's own volume scale (+0x2C) and its result (+0x30) (SoundPlexLeaf::Update),
// Voice's base frequency (+0x28C, VoiceCollection::Create: Sound::getDefaults into it)
constexpr std::uintptr_t kRvaWaveVoiceStore = 0x00397178;
constexpr std::uint8_t kWaveVoiceStoreSig[] = {0x48, 0x89, 0x43, 0x40};
constexpr std::uintptr_t kRvaLeafScale = 0x003BF285;
constexpr std::uint8_t kLeafScaleSig[] = {0xF3, 0x0F, 0x59, 0x41, 0x2C, 0xF3, 0x0F, 0x11, 0x41, 0x30};
constexpr std::uintptr_t kRvaVoiceFrequency = 0x0039DF25;
constexpr std::uint8_t kVoiceFrequencySig[] = {0x48, 0x8D, 0x93, 0x8C, 0x02, 0x00, 0x00};
constexpr std::size_t kWaveScale = 0x2C, kWaveScaled = 0x30, kWaveVoice = 0x40, kVoiceFrequency = 0x28C;
constexpr std::uintptr_t kRvaFmodSystem = 0x0233C4A0;   // cdc::gFMOD (FMOD::System*)
constexpr std::uintptr_t kRvaAudioEngine = 0x0233C4D0;  // cdc::g_AudioEngine; +0x7CE70: its audio update lock
constexpr std::size_t kAudioLock = 0x7CE70;
// cdc::MultiplexStream::Init(this, StreamType, char priority, const char* name, int, SoundPlex*, bool): type 0 (a sound
// stream) opens "<sm_soundStreamDir><name>.mul" (the name copied into the stream, +0x140)
constexpr std::uintptr_t kRvaStreamInit = 0x003A7620;
constexpr std::uint8_t kStreamInitSig[] = {0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x6C, 0x24, 0x10, 0x48, 0x89, 0x74, 0x24, 0x18, 0x57,
                                           0x48, 0x81, 0xEC, 0x50, 0x02, 0x00, 0x00, 0x48, 0x8B, 0xD9, 0x49, 0x8B, 0xF1, 0x48, 0x81, 0xC1};
// (v0.9.476) cdc::Sample::Upload(this, int, const void* fsb, int): FMOD::System::createSound of a bank sound's FSB5
// (mode 0x0A000A40 / 42 with a loop, exinfo length = sample +0x14, frequency +0x24), then getSubSound(0) into +8: what
// cdc::WaveSection::HandleResourceData does with each wave resource after building its sample from the resource's
// 0x14-byte header (frequency, loop start, loop end, priority, volume). Its call of createSound is checked too.
constexpr std::uintptr_t kRvaSampleUpload = 0x003C2E30;
constexpr std::uint8_t kSampleUploadSig[] = {0x48, 0x89, 0x6C, 0x24, 0x20, 0x56, 0x48, 0x81, 0xEC, 0x10, 0x01, 0x00, 0x00, 0x80, 0x3D};
constexpr std::uintptr_t kRvaSampleUploadCreateCall = 0x003C2F03;
constexpr std::size_t kBankHeader = 0x14;
// cdc::SoundOwner: the emitting object's zone ID (set by animation sound triggers, barks, action graph sound nodes)
constexpr std::size_t kOwnerObject = 0x344;
constexpr std::size_t kOwnerRoot = 0x34C;   // the root sound's DTP ID

using WaveCtorFn = void*(__fastcall*)(void*, void*, const std::uint32_t*, void*, const void*, const void*, std::uint8_t*,
                                      std::uint32_t, void*, void*);
using SampleCreateFn = std::uint8_t*(__fastcall*)(int, int, int, int, int, float);
using CreateSoundFn = int(__fastcall*)(void*, const char*, unsigned, void*, void**);
using GetDefaultsFn = int(__fastcall*)(void*, float*, float*, float*, int*);
using StreamInitFn = bool(__fastcall*)(void*, int, char, const char*, int, void*, bool);
using SampleUploadFn = int(__fastcall*)(void*, int, const void*, int);

WaveCtorFn g_origWaveCtor = nullptr;
SampleCreateFn g_sampleCreate = nullptr;
CreateSoundFn g_createSound = nullptr;
GetDefaultsFn g_getDefaults = nullptr;
StreamInitFn g_origStreamInit = nullptr;
SampleUploadFn g_sampleUpload = nullptr;
bool g_installed = false;
bool g_streamHook = false;
bool g_adjustable = false;   // the foley settings' places checked
const int* g_voiceLanguage = nullptr;   // the game's voice language (language_t), when found

// CRC-32/BZIP2 of the lower-cased text, continued (the tiger archives' path hash); no allocation (the audio thread)
std::uint32_t Crc(std::uint32_t h, const char* text) {
    for (; *text; ++text) {
        unsigned char c = static_cast<unsigned char>(*text);
        if (c >= 'A' && c <= 'Z') c = static_cast<unsigned char>(c - 'A' + 'a');
        h ^= std::uint32_t(c) << 24;
        for (int k = 0; k < 8; ++k) h = (h & 0x80000000u) ? (h << 1) ^ 0x04C11DB7u : h << 1;
    }
    return h;
}
std::uint32_t StreamNameHash(const char* name) {
    return ~Crc(Crc(Crc(0xFFFFFFFFu, "pcx64-w\\audio\\streams\\"), name), ".mul");
}

// ---- the table the audio thread reads (immutable once published; old ones are kept) ----
struct Choice {
    std::vector<void*> samples;            // cdc::Sample*, one per file (our reference keeps each alive)
    mutable int last = -1;                 // (audio thread only)
    mutable std::uint32_t seed = 0x9E3779B9u;
    void* Pick() const {
        if (samples.empty()) return nullptr;
        if (samples.size() == 1) return samples[0];
        seed = seed * 1664525u + 1013904223u;
        int pick = int((seed >> 8) % std::uint32_t(samples.size() - 1));
        if (pick >= last && last >= 0) ++pick;   // never the same twice in a row
        last = pick;
        return samples[std::size_t(pick)];
    }
};
struct StreamChoice {
    std::vector<std::string> names;        // the framework archive's stream names (kept with the table)
    mutable int last = -1;
    mutable std::uint32_t seed = 0x2545F491u;
    const char* Pick() const {
        if (names.empty()) return nullptr;
        if (names.size() == 1) return names[0].c_str();
        seed = seed * 1664525u + 1013904223u;
        int pick = int((seed >> 8) % std::uint32_t(names.size() - 1));
        if (pick >= last && last >= 0) ++pick;
        last = pick;
        return names[std::size_t(pick)].c_str();
    }
};
struct Adjust {
    float volume = 1.0f, ratio = 1.0f;     // a volume factor, a frequency factor (2^(semitones/12))
};
struct Table {
    std::unordered_map<std::uint32_t, std::unordered_map<std::uint32_t, Adjust>> adjust;   // zone -> wave -> settings
    std::unordered_map<std::uint32_t, Choice> global;                                       // wave -> choice
    std::unordered_map<std::uint32_t, std::unordered_map<std::uint32_t, Choice>> byObject;  // zone -> wave -> choice
    std::unordered_map<std::uint32_t, std::unordered_map<std::uint32_t, StreamChoice>> streams;   // zone -> stream hash
};
std::atomic<const Table*> g_table{nullptr};
std::atomic<std::uint32_t> g_playerZone{0};

// what the audio thread saw the player's object play: a single-producer ring the game thread drains
struct Heard {
    std::uint32_t object, wave, plex, root;
};
constexpr std::size_t kRing = 256;
std::array<Heard, kRing> g_ring{};
std::atomic<std::size_t> g_ringWrite{0}, g_ringRead{0};

void ReadOwner(const std::uint8_t* owner, std::uint32_t& object, std::uint32_t& root) {
    __try {
        object = *reinterpret_cast<const std::uint32_t*>(owner + kOwnerObject);
        root = *reinterpret_cast<const std::uint32_t*>(owner + kOwnerRoot);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        object = 0;
        root = 0;
    }
}

// A sound just made: its volume scale and its voice's frequency (no C++ objects: SEH).
void ApplyAdjust(std::uint8_t* made, float volume, float ratio) {
    __try {
        if (volume != 1.0f) {
            *reinterpret_cast<float*>(made + kWaveScale) *= volume;
            *reinterpret_cast<float*>(made + kWaveScaled) *= volume;
        }
        if (ratio != 1.0f)
            if (std::uint8_t* voice = *reinterpret_cast<std::uint8_t**>(made + kWaveVoice))
                *reinterpret_cast<float*>(voice + kVoiceFrequency) *= ratio;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

void* __fastcall HookWaveCtor(void* self, void* bucket, const std::uint32_t* wave, void* sample, const void* controls,
                              const void* controls3d, std::uint8_t* owner, std::uint32_t plex, void* buss, void* parent) {
    const Adjust* adjust = nullptr;
    if (wave && owner && sample) {
        std::uint32_t object = 0, root = 0;
        ReadOwner(owner, object, root);
        const std::uint32_t id = *wave;
        if (object && object == g_playerZone.load(std::memory_order_relaxed)) {
            const std::size_t w = g_ringWrite.load(std::memory_order_relaxed);
            if (w - g_ringRead.load(std::memory_order_acquire) < kRing) {
                g_ring[w % kRing] = {object, id, plex, root};
                g_ringWrite.store(w + 1, std::memory_order_release);
            }
        }
        if (const Table* table = g_table.load(std::memory_order_acquire)) {
            const Choice* choice = nullptr;
            if (object)
                if (auto set = table->byObject.find(object); set != table->byObject.end())
                    if (auto it = set->second.find(id); it != set->second.end()) choice = &it->second;
            if (!choice)
                if (auto it = table->global.find(id); it != table->global.end()) choice = &it->second;
            if (choice)
                if (void* mine = choice->Pick()) sample = mine;
            if (object && g_adjustable)
                if (auto set = table->adjust.find(object); set != table->adjust.end())
                    if (auto it = set->second.find(id); it != set->second.end()) adjust = &it->second;
        }
    }
    void* made = g_origWaveCtor(self, bucket, wave, sample, controls, controls3d, owner, plex, buss, parent);
    if (adjust && made) ApplyAdjust(static_cast<std::uint8_t*>(made), adjust->volume, adjust->ratio);
    return made;
}

bool __fastcall HookStreamInit(void* self, int type, char priority, const char* name, int a5, void* plex, bool a7) {
    if (type == 0 && name)
        if (const Table* table = g_table.load(std::memory_order_acquire); table && !table->streams.empty())
            if (auto set = table->streams.find(g_playerZone.load(std::memory_order_relaxed)); set != table->streams.end())
                if (auto it = set->second.find(StreamNameHash(name)); it != set->second.end())
                    if (const char* mine = it->second.Pick()) name = mine;
    return g_origStreamInit(self, type, priority, name, a5, plex, a7);
}

// FMOD::System::createSound under the audio engine's lock (no C++ objects here: structured exception handling).
int CreateSoundLocked(void* system, const char* data, unsigned mode, void* exinfo, void** sound) {
    auto* lock = reinterpret_cast<CRITICAL_SECTION*>(GameBuild::Base() + kRvaAudioEngine + kAudioLock);
    int result = -1;
    EnterCriticalSection(lock);   // (the audio thread only tries it: it skips a tick)
    __try {
        result = g_createSound(system, data, mode, exinfo, sound);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        result = -2;
    }
    LeaveCriticalSection(lock);
    return result;
}

// ---- the game thread's side ----
int g_language = -1;   // the voice language the table was made for
std::vector<Entry> g_global;
std::map<std::uint32_t, std::vector<Entry>> g_voices;
std::map<std::uint32_t, std::vector<Adjustment>> g_adjustments;
bool g_dirty = false;
std::map<std::string, void*> g_samples;   // file (path) -> its cdc::Sample* (nullptr: could not be made)
std::set<std::uint32_t> g_logged;
fs::path g_logPath;

// ---- (v0.9.476) the worn outfit's bank sounds in the voice language chosen in game ----
// Lara's grunts are bank sounds (waves) in her outfit object's .drm, one section per language copy (the most languages
// share one; Italian, Japanese, Polish and Russian have their own). Only the voice language's copy is loaded with the
// object (0.9.455's section check), and nothing of a loaded object is read again, so a voice language chosen in game
// changed the dialogue (streamed) but not the grunts. Now, while the voice language differs from the one the worn
// outfit's object was first seen in, the object's .drm is read on a thread (the game's archives, GameArchive), and
// for each of its waves whose copy for the voice language differs from the copy of the loaded language, that copy is
// read; the game thread makes a cdc::Sample of it as the game does (Sample::Create from its header, Sample::Upload of
// its FSB5, the audio engine's lock held) and the wave constructor hook plays it for that object (a mod's voice for the
// same wave wins). Back to the loaded language, they are dropped.
struct LanguageCopy {
    int language = -1;
    std::unordered_map<std::uint32_t, void*> samples;   // wave -> cdc::Sample* (kept for the session)
};
std::map<std::uint32_t, LanguageCopy> g_copies;          // zone -> the copies played for it now
std::map<std::uint32_t, int> g_zoneLanguage;              // zone -> the voice language it was first seen in
std::set<std::pair<std::uint32_t, int>> g_copyTried;      // (zone, language) read already
std::map<std::pair<std::uint32_t, int>, std::unordered_map<std::uint32_t, void*>> g_copyCache;
struct CopyJob {
    std::uint32_t zone = 0;
    int language = -1, loaded = -1;
    std::string object;
    std::vector<std::pair<std::uint32_t, std::vector<std::uint8_t>>> waves;
    int multi = 0;   // waves with copies per language
    std::string note;
    std::atomic_bool done{false};
};
std::unique_ptr<CopyJob> g_copyJob;   // (its thread detached: it only sets done)

fs::path GameFolder() {
    wchar_t path[MAX_PATH]{};
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    return fs::path(path).parent_path();
}

// A cdc::Sample of a WAV file (FMOD decodes it into memory once).
void* MakeSample(const fs::path& file) {
    const std::string key = file.string();
    if (auto it = g_samples.find(key); it != g_samples.end()) return it->second;
    void*& made = g_samples[key];
    std::ifstream in(file, std::ios::binary);
    const std::vector<char> data((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (data.size() < 44) {
        Log::Writef("Sounds: %s could not be read", key.c_str());
        return nullptr;
    }
    void* system = *reinterpret_cast<void**>(GameBuild::Base() + kRvaFmodSystem);
    if (!system) return nullptr;
    alignas(8) std::uint8_t exinfo[0xD8]{};   // FMOD_CREATESOUNDEXINFO (FMOD Ex 4.44, x64)
    *reinterpret_cast<int*>(exinfo) = int(sizeof(exinfo));
    *reinterpret_cast<unsigned*>(exinfo + 4) = unsigned(data.size());   // length
    // FMOD_LOOP_OFF | FMOD_2D | FMOD_SOFTWARE | FMOD_CREATESAMPLE | FMOD_OPENMEMORY | FMOD_LOWMEM (the game places it)
    const unsigned mode = 0x1 | 0x8 | 0x40 | 0x100 | 0x800 | 0x08000000;
    void* sound = nullptr;
    const int result = CreateSoundLocked(system, data.data(), mode, exinfo, &sound);
    if (result != 0 || !sound) {
        Log::Writef("Sounds: %s could not be opened (FMOD error %d; use a WAV file)", key.c_str(), result);
        return nullptr;
    }
    float frequency = 44100.0f;
    if (g_getDefaults) g_getDefaults(sound, &frequency, nullptr, nullptr, nullptr);
    std::uint8_t* sample = g_sampleCreate(0 /* kWave */, int(data.size()), 0, 0, int(frequency), 1.0f);
    if (!sample) return nullptr;
    *reinterpret_cast<void**>(sample + 0x00) = sound;   // m_pSoundHandle
    *reinterpret_cast<void**>(sample + 0x08) = sound;   // m_pSoundPlay
    made = sample;
    return sample;
}

Choice MakeChoice(const Entry& entry) {
    Choice choice;
    for (const auto& file : entry.files)
        if (void* sample = MakeSample(file)) choice.samples.push_back(sample);
    choice.seed ^= entry.wave * 2654435761u;
    return choice;
}

// Whether an entry plays now: one for the voice language, or the one for every language when that one has none.
bool Wanted(const Entry& e, const std::vector<Entry>& all) {
    if (e.language >= 0) return e.language == g_language;
    return std::none_of(all.begin(), all.end(), [&](const Entry& x) {
        return x.language >= 0 && x.language == g_language && x.wave == e.wave && x.stream == e.stream;
    });
}

void Publish() {
    auto* table = new Table();   // (never freed: the audio thread may still read an old one; they are small)
    std::size_t ofLanguage = 0;
    for (const auto& e : g_global)
        if (Wanted(e, g_global))
            if (Choice c = MakeChoice(e); !c.samples.empty()) {
                table->global[e.wave] = std::move(c);
                ofLanguage += e.language >= 0;
            }
    std::size_t streams = 0;
    for (const auto& [zone, entries] : g_voices)
        for (const auto& e : entries) {
            if (!Wanted(e, entries)) continue;
            ofLanguage += e.language >= 0;
            if (e.stream) {
                if (!g_streamHook || e.names.empty()) continue;
                StreamChoice c;
                c.names = e.names;
                c.seed ^= e.wave;
                table->streams[zone][e.wave] = std::move(c);
                ++streams;
                continue;
            }
            if (Choice c = MakeChoice(e); !c.samples.empty()) table->byObject[zone][e.wave] = std::move(c);
        }
    std::size_t copied = 0;   // (v0.9.476) the worn outfit's bank sounds in the voice language chosen in game
    for (const auto& [zone, copy] : g_copies) {
        auto& set = table->byObject[zone];
        for (const auto& [wave, sample] : copy.samples)
            if (!set.count(wave)) {
                Choice c;
                c.samples.push_back(sample);
                set[wave] = std::move(c);
                ++copied;
            }
    }
    if (copied) Log::Detailf("Sounds: %zu of the worn outfit's bank sounds played in the voice language chosen in game", copied);
    std::size_t adjusted = 0;
    for (const auto& [zone, list] : g_adjustments)
        for (const auto& a : list) {
            if (a.volume == 1.0f && a.pitch == 0.0f) continue;
            // (several settings of one sound, a group's and all footsteps' say: their volumes multiplied, pitches added)
            Adjust& x = table->adjust[zone][a.wave];
            x.volume *= std::max(0.0f, a.volume);
            x.ratio *= std::pow(2.0f, std::clamp(a.pitch, -48.0f, 48.0f) / 12.0f);
            ++adjusted;
        }
    g_table.store(table, std::memory_order_release);
    if (adjusted)
        Log::Detailf("Sounds: %zu foley setting(s) (volume, pitch) for the worn outfits%s", adjusted,
                    g_adjustable ? "" : ": not applied (the game's sound code is not where this framework expects it)");
    std::size_t voices = 0;
    for (const auto& [zone, set] : table->byObject) voices += set.size();
    Log::Detailf("Sounds: %zu wave(s) replaced for every emitter, %zu wave(s) and %zu stream(s) for %zu outfit object(s); "
                "voice language %s (%zu of its own)",
                table->global.size(), voices, streams, g_voices.size(), g_language >= 0 ? LanguageName(g_language) : "unknown",
                ofLanguage);
}

int ReadLanguage(const int* at) {
    __try {
        return *at;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
}
}  // namespace

std::uint32_t StreamHash(const std::string& name) { return StreamNameHash(name.c_str()); }

namespace {
const char* const kLanguages[] = {"english", "french", "german", "italian", "latamspanish", "iberspanish", "japanese", "portuguese",
                                  "polish", "russian", "dutch", "korean", "chinese", "simplechinese", "ksa_english", "arabic"};
}
int LanguageFromName(const std::string& name) {
    if (name.empty()) return -1;
    if (name.find_first_not_of("0123456789") == std::string::npos) {
        const int n = std::atoi(name.c_str());
        return n >= 0 && n < int(std::size(kLanguages)) ? n : -1;
    }
    std::string lower = name;
    for (char& c : lower) c = char(std::tolower(static_cast<unsigned char>(c)));
    for (int i = 0; i < int(std::size(kLanguages)); ++i)
        if (lower == kLanguages[i]) return i;
    return -1;
}
const char* LanguageName(int language) {
    return language >= 0 && language < int(std::size(kLanguages)) ? kLanguages[language] : "?";
}
const int* LanguageVariable(std::uintptr_t getterRva);
int TextLanguage() {
    static const int* text = LanguageVariable(kRvaGetTextLanguage);
    if (!text) return -1;
    const int language = ReadLanguage(text);
    return language >= 0 && language < int(std::size(kLanguages)) ? language : -1;
}
int VoiceLanguage() {
    if (!g_voiceLanguage) return -1;
    const int language = ReadLanguage(g_voiceLanguage);
    return language >= 0 && language < int(std::size(kLanguages)) ? language : -1;
}

void AddGlobal(const std::vector<Entry>& entries, const std::string& mod) {
    for (const auto& e : entries) {
        if (e.stream) continue;   // (streams for every listener are replaced in the archive: mod.json "streams")
        if (std::any_of(g_global.begin(), g_global.end(), [&](const Entry& x) { return x.wave == e.wave && x.language == e.language; })) {
            Log::Writef("Sounds: %s: wave %u is already replaced by another mod", mod.c_str(), e.wave);
            continue;
        }
        g_global.push_back(e);
    }
    g_dirty = true;
}

void SetVoices(const std::map<std::uint32_t, std::vector<Entry>>& voices) {
    auto same = [&]() {
        if (voices.size() != g_voices.size()) return false;
        for (const auto& [zone, entries] : voices) {
            auto it = g_voices.find(zone);
            if (it == g_voices.end() || it->second.size() != entries.size()) return false;
            for (std::size_t i = 0; i < entries.size(); ++i)
                if (entries[i].wave != it->second[i].wave || entries[i].files != it->second[i].files ||
                    entries[i].stream != it->second[i].stream || entries[i].names != it->second[i].names ||
                    entries[i].language != it->second[i].language)
                    return false;
        }
        return true;
    };
    if (same()) return;
    g_voices = voices;
    g_dirty = true;
}

void SetAdjustments(const std::map<std::uint32_t, std::vector<Adjustment>>& adjustments) {
    auto same = [&]() {
        if (adjustments.size() != g_adjustments.size()) return false;
        for (const auto& [zone, list] : adjustments) {
            const auto it = g_adjustments.find(zone);
            if (it == g_adjustments.end() || it->second.size() != list.size()) return false;
            for (std::size_t i = 0; i < list.size(); ++i)
                if (list[i].wave != it->second[i].wave || list[i].volume != it->second[i].volume || list[i].pitch != it->second[i].pitch)
                    return false;
        }
        return true;
    };
    if (same()) return;
    g_adjustments = adjustments;
    g_dirty = true;
}

const int* LanguageVariable(std::uintptr_t getterRva) {
    const std::uintptr_t base = GameBuild::Base();
    if (!GameBuild::VerifyBytes(getterRva, kGetVoiceLanguageSig, sizeof(kGetVoiceLanguageSig)) ||
        *reinterpret_cast<const std::uint8_t*>(base + getterRva + 6) != 0xC3)
        return nullptr;
    const std::int32_t disp = *reinterpret_cast<const std::int32_t*>(base + getterRva + 2);
    return reinterpret_cast<const int*>(base + getterRva + 6 + disp);
}

// Executable memory within +-2 GB of the game's code (for a rel32 call).
void* AllocNearGame(std::size_t size) {
    const std::uintptr_t base = GameBuild::Base();
    SYSTEM_INFO si{};
    GetSystemInfo(&si);
    const std::uintptr_t gran = std::max<std::uintptr_t>(si.dwAllocationGranularity, 0x10000u);
    for (std::uintptr_t cursor = (base + 0x04000000ull + gran - 1) & ~(gran - 1); cursor < base + 0x60000000ull;) {
        MEMORY_BASIC_INFORMATION mbi{};
        if (!VirtualQuery(reinterpret_cast<const void*>(cursor), &mbi, sizeof(mbi))) break;
        const std::uintptr_t regionBase = reinterpret_cast<std::uintptr_t>(mbi.BaseAddress);
        const std::uintptr_t regionEnd = regionBase + mbi.RegionSize;
        if (mbi.State == MEM_FREE) {
            const std::uintptr_t candidate = (std::max(cursor, regionBase) + gran - 1) & ~(gran - 1);
            if (candidate < regionEnd && regionEnd - candidate >= size)
                if (void* p = VirtualAlloc(reinterpret_cast<void*>(candidate), size, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE))
                    return p;
        }
        if (regionEnd <= cursor) break;
        cursor = (regionEnd + gran - 1) & ~(gran - 1);
    }
    return nullptr;
}

bool g_bankLanguageInstalled = false;
void InstallBankLanguage() {
    const std::uintptr_t base = GameBuild::Base();
    const int* text = LanguageVariable(kRvaGetTextLanguage);
    const int* voice = LanguageVariable(kRvaGetVoiceLanguage);
    if (!text || !voice || text == voice ||
        !GameBuild::VerifyBytes(kRvaSectionLocaleCheck, kSectionLocaleCheckSig, sizeof(kSectionLocaleCheckSig)) ||
        !GameBuild::VerifyBytes(kRvaSectionLocaleCheck - 0x1B, kSectionLocaleCheckBefore, sizeof(kSectionLocaleCheckBefore))) {
        Log::Write("Sounds: the game's .drm section check is not where this framework expects it: bank sounds stay in the "
                   "text language");
        return;
    }
    auto* stub = static_cast<std::uint8_t*>(AllocNearGame(0x1000));
    const std::intptr_t rel = stub ? reinterpret_cast<std::intptr_t>(stub) - std::intptr_t(base + kRvaSectionLocaleCheck + 5) : 0;
    if (!stub || rel < INT32_MIN || rel > INT32_MAX) {
        if (stub) VirtualFree(stub, 0, MEM_RELEASE);
        Log::Write("Sounds: no memory near the game's code: bank sounds stay in the text language");
        return;
    }
    std::memcpy(stub, kBankLocaleStub, sizeof(kBankLocaleStub));
    const std::uint64_t textAt = reinterpret_cast<std::uint64_t>(text), voiceAt = reinterpret_cast<std::uint64_t>(voice);
    std::memcpy(stub + kBankStubTextAt, &textAt, sizeof(textAt));
    std::memcpy(stub + kBankStubVoiceAt, &voiceAt, sizeof(voiceAt));
    FlushInstructionCache(GetCurrentProcess(), stub, sizeof(kBankLocaleStub));
    std::uint8_t call[5] = {0xE8};
    const std::int32_t rel32 = static_cast<std::int32_t>(rel);
    std::memcpy(call + 1, &rel32, sizeof(rel32));
    auto* at = reinterpret_cast<std::uint8_t*>(base + kRvaSectionLocaleCheck);
    DWORD old = 0;
    if (!VirtualProtect(at, sizeof(call), PAGE_EXECUTE_READWRITE, &old)) {
        VirtualFree(stub, 0, MEM_RELEASE);
        return;
    }
    std::memcpy(at, call, sizeof(call));
    FlushInstructionCache(GetCurrentProcess(), at, sizeof(call));
    VirtualProtect(at, sizeof(call), old, &old);
    g_bankLanguageInstalled = true;
    Log::Detail("Sounds: bank sounds (Lara's grunts) are loaded in the voice language, not the text language");
}

bool Install() {
    if (g_installed) return true;
    if (!g_bankLanguageInstalled) InstallBankLanguage();   // (v0.9.455: whatever else is found)
    if (!GameBuild::VerifyBytes(kRvaWaveCtorCall, kWaveCtorCallSig, sizeof(kWaveCtorCallSig)) ||
        !GameBuild::VerifyBytes(kRvaSampleCreate, kSampleCreateSig, sizeof(kSampleCreateSig)) ||
        !GameBuild::VerifyBytes(kRvaCreateSound, kCreateSoundSig, sizeof(kCreateSoundSig)) ||
        !GameBuild::VerifyBytes(kRvaGetDefaults, kGetDefaultsSig, sizeof(kGetDefaultsSig)) ||
        !GameBuild::VerifyBytes(kRvaWaveCtor, kWaveCtorSig, sizeof(kWaveCtorSig))) {
        Log::Write("Sounds: the game's sound functions are not where this framework expects them: mods' sounds are off");
        return false;
    }
    const std::uintptr_t base = GameBuild::Base();
    // the voice language (mov eax, [rip + disp32]; ret): its variable
    if (GameBuild::VerifyBytes(kRvaGetVoiceLanguage, kGetVoiceLanguageSig, sizeof(kGetVoiceLanguageSig)) &&
        *reinterpret_cast<const std::uint8_t*>(base + kRvaGetVoiceLanguage + 6) == 0xC3) {
        const std::int32_t disp = *reinterpret_cast<const std::int32_t*>(base + kRvaGetVoiceLanguage + 2);
        g_voiceLanguage = reinterpret_cast<const int*>(base + kRvaGetVoiceLanguage + 6 + disp);
    } else {
        Log::Write("Sounds: the game's voice language is not where this framework expects it: sounds for one language are off");
    }
    g_sampleCreate = reinterpret_cast<SampleCreateFn>(base + kRvaSampleCreate);
    g_createSound = reinterpret_cast<CreateSoundFn>(base + kRvaCreateSound);
    g_getDefaults = reinterpret_cast<GetDefaultsFn>(base + kRvaGetDefaults);
    if (GameBuild::VerifyBytes(kRvaSampleUpload, kSampleUploadSig, sizeof(kSampleUploadSig)) &&
        GameBuild::VerifyBytes(kRvaSampleUploadCreateCall, reinterpret_cast<const std::uint8_t*>("\xE8"), 1) &&
        kRvaSampleUploadCreateCall + 5 +
                static_cast<std::intptr_t>(*reinterpret_cast<const std::int32_t*>(base + kRvaSampleUploadCreateCall + 1)) ==
            kRvaCreateSound) {
        g_sampleUpload = reinterpret_cast<SampleUploadFn>(base + kRvaSampleUpload);
    } else {
        Log::Write("Sounds: cdc::Sample::Upload is not where this framework expects it: Lara's grunts keep the voice language the level loaded");
    }
    if (MH_CreateHook(reinterpret_cast<void*>(base + kRvaWaveCtor), reinterpret_cast<void*>(&HookWaveCtor),
                      reinterpret_cast<void**>(&g_origWaveCtor)) != MH_OK) {
        Log::Write("Sounds: the sound hook could not be created");
        return false;
    }
    // the stream hook (outfits' death screams, panic, breathing): only with its function where expected
    if (GameBuild::VerifyBytes(kRvaStreamInit, kStreamInitSig, sizeof(kStreamInitSig)) &&
        MH_CreateHook(reinterpret_cast<void*>(base + kRvaStreamInit), reinterpret_cast<void*>(&HookStreamInit),
                      reinterpret_cast<void**>(&g_origStreamInit)) == MH_OK) {
        g_streamHook = true;
        // (the Modding Suite looks for this text: outfit voices may list streams)
        Log::Detail("Sounds: outfits' voice streams (death screams, panic, breathing) are played instead of the game's");
    } else {
        Log::Write("Sounds: the game's stream function is not where this framework expects it: outfits' voice streams are off");
    }
    g_adjustable = GameBuild::VerifyBytes(kRvaWaveVoiceStore, kWaveVoiceStoreSig, sizeof(kWaveVoiceStoreSig)) &&
                   GameBuild::VerifyBytes(kRvaLeafScale, kLeafScaleSig, sizeof(kLeafScaleSig)) &&
                   GameBuild::VerifyBytes(kRvaVoiceFrequency, kVoiceFrequencySig, sizeof(kVoiceFrequencySig));
    // (the Modding Suite looks for this text: outfits' foley settings)
    if (g_adjustable) Log::Detail("Sounds: outfits' foley settings (volume, pitch) are applied to the sounds of the outfit's object");
    else Log::Write("Sounds: the game's sound code is not where this framework expects it: outfits' foley settings are off");
    g_installed = true;
    g_logPath = GameFolder() / L"ROTTRFramework" / L"lara_sounds.log";
    // (the waves already logged, so a new session adds only new ones)
    if (std::ifstream in(g_logPath); in) {
        std::string line;
        while (std::getline(in, line))
            if (line.rfind("wave ", 0) == 0) g_logged.insert(std::uint32_t(std::strtoul(line.c_str() + 5, nullptr, 10)));
    }
    // (the Modding Suite looks for this text in the .asi to tell a framework that plays mods' sounds)
    Log::Detail("Sounds: mods' waves and outfit voices are played instead of the game's");
    return true;
}


namespace {
// ---- (v0.9.476) the worn outfit's bank sounds in the voice language chosen in game: the reading and the samples ----
// (the copy a language plays: the most specific section with its bit; the latest archive's when equally specific)
int CopyIndexFor(const GameArchive::ArchiveSet& set, const GameArchive::Drm& drm, const std::vector<std::size_t>& sections,
                 int language) {
    int best = -1;
    auto bits = [](std::uint32_t l) {
        int n = 0;
        for (int b = 0; b < 14; ++b) n += (l >> b) & 1;
        return n;
    };
    for (const std::size_t i : sections) {
        const std::uint32_t locale = drm.ids[i].locale;
        if (!(locale & (1u << language))) continue;
        if (best < 0) {
            best = int(i);
            continue;
        }
        const std::uint32_t other = drm.ids[std::size_t(best)].locale;
        if (bits(locale) < bits(other) ||
            (bits(locale) == bits(other) && set.RankOf(drm.locations[i].archiveId) > set.RankOf(drm.locations[std::size_t(best)].archiveId)))
            best = int(i);
    }
    return best;
}

void ReadCopies(CopyJob* job) {
    static std::mutex mutex;
    static std::unique_ptr<GameArchive::ArchiveSet> set;
    std::lock_guard<std::mutex> lock(mutex);
    char text[256];
    if (!set) {
        auto fresh = std::make_unique<GameArchive::ArchiveSet>();
        std::string error;
        if (!fresh->Open(GameFolder(), error, nullptr, false)) {
            job->note = "the game's archives could not be read: " + error;
            job->done.store(true);
            return;
        }
        set = std::move(fresh);
    }
    const std::string path = "pcx64-w\\" + GameArchive::Lower(job->object) + ".drm";
    std::vector<std::uint8_t> bytes;
    GameArchive::Drm drm;
    if (!set->ReadFile(path, bytes) || !drm.Parse(bytes)) {
        job->note = path + " could not be read";
        job->done.store(true);
        return;
    }
    std::map<std::uint32_t, std::vector<std::size_t>> banks;
    for (std::size_t i = 0; i < drm.ids.size(); ++i)
        if (drm.ids[i].type == GameArchive::kTypeSoundBank && set->HasArchive(drm.locations[i].archiveId))
            banks[std::uint32_t(drm.ids[i].id)].push_back(i);
    int failed = 0;
    for (const auto& [wave, sections] : banks) {
        if (sections.size() < 2) continue;
        ++job->multi;
        const int want = CopyIndexFor(*set, drm, sections, job->language);
        const int have = CopyIndexFor(*set, drm, sections, job->loaded);
        if (want < 0 || want == have) continue;
        std::vector<std::uint8_t> data;
        const std::size_t i = std::size_t(want);
        if (!set->ReadBlob(drm.LocationOf(i), data, drm.TotalSize(i)) || data.size() < kBankHeader + 0x40 ||
            std::memcmp(data.data() + kBankHeader, "FSB5", 4) != 0) {
            ++failed;
            continue;
        }
        job->waves.emplace_back(wave, std::move(data));
    }
    std::snprintf(text, sizeof(text), "%s: %zu bank sound(s), %d with copies per language, %zu to play in %s%s", path.c_str(),
                  banks.size(), job->multi, job->waves.size(), LanguageName(job->language),
                  failed ? (" (" + std::to_string(failed) + " could not be read)").c_str() : "");
    job->note = text;
    job->done.store(true);
}

// A cdc::Sample of a wave resource (its 0x14-byte header, then its FSB5), as cdc::WaveSection makes one.
int UploadLocked(void* sample, const std::uint8_t* fsb, int size) {
    auto* lock = reinterpret_cast<CRITICAL_SECTION*>(GameBuild::Base() + kRvaAudioEngine + kAudioLock);
    int result = 0;
    EnterCriticalSection(lock);
    __try {
        result = g_sampleUpload(sample, 0, fsb, size);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        result = -1;
    }
    LeaveCriticalSection(lock);
    return result;
}

void* MakeBankSample(const std::vector<std::uint8_t>& data) {
    if (!g_sampleUpload || !g_sampleCreate || data.size() <= kBankHeader) return nullptr;
    std::uint32_t frequency = 0, loopStart = 0, loopEnd = 0;
    float volume = 1.0f;
    std::memcpy(&frequency, data.data(), 4);
    std::memcpy(&loopStart, data.data() + 4, 4);
    std::memcpy(&loopEnd, data.data() + 8, 4);
    std::memcpy(&volume, data.data() + 0x10, 4);
    const int size = int(data.size() - kBankHeader);
    std::uint8_t* sample = g_sampleCreate(0 /* kWave */, size, int(loopStart), int(loopEnd), int(frequency), volume);
    if (!sample) return nullptr;
    if (UploadLocked(sample, data.data() + kBankHeader, size) <= 0) return nullptr;   // (left: tiny, and never played)
    return sample;
}

// game thread, every tick
void TickCopies(std::uint32_t zone, int voice) {
    if (g_copyJob && g_copyJob->done.load()) {   // a reading done: its samples
        CopyJob& job = *g_copyJob;
        auto& made = g_copyCache[{job.zone, job.language}];
        int failed = 0;
        for (const auto& [wave, data] : job.waves) {
            if (made.count(wave)) continue;
            if (void* sample = MakeBankSample(data)) made[wave] = sample;
            else ++failed;
        }
        Log::Writef("Sounds: the worn outfit (%s, object %u) in the voice language %s: %s; %zu sample(s) made%s", job.object.c_str(),
                    job.zone, LanguageName(job.language), job.note.c_str(), made.size(),
                    failed ? (", " + std::to_string(failed) + " failed").c_str() : "");
        g_copyJob.reset();
    }
    if (!zone || voice < 0) return;
    const auto first = g_zoneLanguage.emplace(zone, voice).first;
    const int loaded = first->second;
    const int wanted = voice != loaded ? voice : -1;
    auto current = g_copies.find(zone);
    if (wanted < 0) {
        if (current != g_copies.end()) {
            g_copies.erase(current);
            g_dirty = true;
            Log::Writef("Sounds: the voice language is %s again, the worn outfit's own: its bank sounds as loaded", LanguageName(voice));
        }
        return;
    }
    if (current != g_copies.end() && current->second.language == wanted) return;
    if (auto cached = g_copyCache.find({zone, wanted}); cached != g_copyCache.end()) {
        LanguageCopy copy;
        copy.language = wanted;
        copy.samples = cached->second;
        g_copies[zone] = std::move(copy);
        g_dirty = true;
        return;
    }
    if (g_copyJob || !g_sampleUpload || g_copyTried.count({zone, wanted})) return;
    const char* name = ZoneList::Name(zone);
    if (!name || !name[0]) return;
    g_copyTried.insert({zone, wanted});
    g_copyJob = std::make_unique<CopyJob>();
    g_copyJob->zone = zone;
    g_copyJob->language = wanted;
    g_copyJob->loaded = loaded;
    g_copyJob->object = name;
    std::thread(ReadCopies, g_copyJob.get()).detach();
    Log::Writef("Sounds: the voice language changed to %s in game (the worn outfit %s was loaded in %s): reading its bank "
                "sounds in %s", LanguageName(wanted), name, LanguageName(loaded), LanguageName(wanted));
}
}  // namespace

void Tick() {
    if (!g_installed) return;
    g_playerZone.store(Features::CurrentOutfitObjectId(), std::memory_order_relaxed);
    if (const int language = VoiceLanguage(); language != g_language) {   // (the options' voice language changed)
        g_language = language;
        g_dirty = true;
    }
    TickCopies(g_playerZone.load(std::memory_order_relaxed), g_language);   // (v0.9.476)
    if (g_dirty && *reinterpret_cast<void**>(GameBuild::Base() + kRvaFmodSystem)) {
        g_dirty = false;
        Publish();
    }
    // the player's sounds, each wave once
    std::size_t r = g_ringRead.load(std::memory_order_relaxed);
    const std::size_t w = g_ringWrite.load(std::memory_order_acquire);
    if (r == w) return;
    std::ofstream log;
    for (; r != w; ++r) {
        const Heard h = g_ring[r % kRing];
        if (!g_logged.insert(h.wave).second) continue;
        if (!log.is_open()) log.open(g_logPath, std::ios::app);
        log << "wave " << h.wave << " plex " << h.plex << " root " << h.root << " object " << h.object << "\n";
    }
    g_ringRead.store(r, std::memory_order_release);
}
}  // namespace SoundReplace
