// LaraFM: your own music on a radio in the game (larafm.h).
#include "larafm.h"

#include "features.h"
#include "game_build.h"
#include "log.h"
#include "overlay.h"
#include "swim_breath.h"

#include <MinHook.h>
#include <Windows.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <shellapi.h>
#include <imgui.h>
#include <intrin.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <memory>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <vector>

namespace LaraFM {
namespace {
// ---------------------------------------------------------------- FMOD Ex 4.44.46 (the game's) --------------------------
using FmodResult = int;
constexpr FmodResult kFmodOk = 0;
constexpr unsigned kModeLoopOff = 0x1, kMode2D = 0x8, kModeSoftware = 0x40, kModeCreateStream = 0x80,
                   kModeOpenUser = 0x400, kModeOpenMemoryPoint = 0x10000000;
constexpr int kFormatPcm16 = 2;
constexpr unsigned kTimeUnitMs = 0x1;
constexpr int kChannelFree = -1;

struct FmodCreateSoundExInfo {
    int cbsize;
    unsigned length;
    unsigned fileoffset;
    int numchannels;
    int defaultfrequency;
    int format;
    unsigned decodebuffersize;
    int initialsubsound;
    int numsubsounds;
    int* inclusionlist;
    int inclusionlistnum;
    void* pcmreadcallback;
    void* pcmsetposcallback;
    void* nonblockcallback;
    const char* dlsname;
    const char* encryptionkey;
    int maxpolyphony;
    void* userdata;
    int suggestedsoundtype;
    void* useropen;
    void* userclose;
    void* userread;
    void* userseek;
    void* userasyncread;
    void* userasynccancel;
    int speakermap;
    void* initialsoundgroup;
    unsigned initialseekposition;
    unsigned initialseekpostype;
    int ignoresetfilesystem;
    int cddaforceaspi;
    unsigned audioqueuepolicy;
    unsigned minmidigranularity;
    int nonblockthreadid;
};
static_assert(sizeof(FmodCreateSoundExInfo) == 0xD8, "FMOD_CREATESOUNDEXINFO (4.44)");
static_assert(offsetof(FmodCreateSoundExInfo, numchannels) == 0x0C && offsetof(FmodCreateSoundExInfo, format) == 0x14 &&
                  offsetof(FmodCreateSoundExInfo, pcmreadcallback) == 0x38 &&
                  offsetof(FmodCreateSoundExInfo, pcmsetposcallback) == 0x40,
              "FMOD_CREATESOUNDEXINFO (4.44)");

struct FmodDspDescription {
    char name[32];
    unsigned version;
    int channels;
    void* create;
    void* release;
    void* reset;
    void* read;
    void* setposition;
    int numparameters;
    void* paramdesc;
    void* setparameter;
    void* getparameter;
    void* config;
    int configwidth;
    int configheight;
    void* userdata;
};
static_assert(offsetof(FmodDspDescription, read) == 0x40 && offsetof(FmodDspDescription, userdata) == 0x80,
              "FMOD_DSP_DESCRIPTION (4.44)");

using SystemUpdateFn = FmodResult(__fastcall*)(void* system);
using SystemCreateSoundFn = FmodResult(__fastcall*)(void* system, const char* name, unsigned mode,
                                                    FmodCreateSoundExInfo* exinfo, void** sound);
using SystemPlaySoundFn = FmodResult(__fastcall*)(void* system, int channelId, void* sound, bool paused, void** channel);
using SystemCreateDspFn = FmodResult(__fastcall*)(void* system, FmodDspDescription* description, void** dsp);
using ChannelAddDspFn = FmodResult(__fastcall*)(void* channel, void* dsp, void** connection);
using ChannelGetPositionFn = FmodResult(__fastcall*)(void* channel, unsigned* position, unsigned unit);
using ChannelSetPausedFn = FmodResult(__fastcall*)(void* channel, bool paused);
using ChannelSetSpeakerMixFn = FmodResult(__fastcall*)(void* channel, const float* levels);
using ChannelSetVolumeFn = FmodResult(__fastcall*)(void* channel, float volume);
using ChannelStopFn = FmodResult(__fastcall*)(void* channel);
using SoundGetLengthFn = FmodResult(__fastcall*)(void* sound, unsigned* length, unsigned unit);
using SoundReleaseFn = FmodResult(__fastcall*)(void* sound);
using SoundSetDefaultsFn = FmodResult(__fastcall*)(void* sound, float frequency, float volume, float pan, int priority);
using DspReleaseFn = FmodResult(__fastcall*)(void* dsp);
using DspRemoveFn = FmodResult(__fastcall*)(void* dsp);

struct FmodFunction {
    const char* name;
    std::uintptr_t rva;
    std::uint8_t bytes[20];
};
// (retail; the first 20 bytes of each: their validate calls included where they come that early)
constexpr FmodFunction kFunctions[] = {
    {"SystemUpdate", 0x00A5ABA0ull, {0x48, 0x83, 0xEC, 0x28, 0x48, 0x8D, 0x54, 0x24, 0x38, 0xE8, 0x72, 0x1F, 0x01, 0x00, 0x85, 0xC0, 0x75, 0x0A, 0x48, 0x8B}},
    {"SystemCreateSound", 0x00A5A2C0ull, {0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x74, 0x24, 0x10, 0x57, 0x48, 0x83, 0xEC, 0x40, 0x48, 0x8B, 0xF2, 0x49, 0x8B}},
    {"SystemPlaySound", 0x00A5A890ull, {0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x74, 0x24, 0x10, 0x57, 0x48, 0x83, 0xEC, 0x40, 0x8B, 0xF2, 0x41, 0x0F, 0xB6}},
    {"SystemCreateDsp", 0x00A5A240ull, {0x48, 0x89, 0x5C, 0x24, 0x08, 0x57, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x8B, 0xFA, 0x49, 0x8B, 0xD8, 0x48, 0x8D, 0x54, 0x24}},
    {"ChannelAddDsp", 0x00A5AFF0ull, {0x48, 0x89, 0x5C, 0x24, 0x08, 0x57, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x8B, 0xFA, 0x49, 0x8B, 0xD8, 0x48, 0x8D, 0x54, 0x24}},
    {"ChannelGetPosition", 0x00A5B0F0ull, {0x48, 0x89, 0x5C, 0x24, 0x08, 0x57, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x8B, 0xDA, 0x41, 0x8B, 0xF8, 0x48, 0x8D, 0x54, 0x24}},
    {"ChannelSetPaused", 0x00A5B2E0ull, {0x40, 0x53, 0x48, 0x83, 0xEC, 0x20, 0x0F, 0xB6, 0xDA, 0x48, 0x8D, 0x54, 0x24, 0x40, 0xE8, 0xCD, 0x67, 0x00, 0x00, 0x85}},
    {"ChannelSetSpeakerMix", 0x00A5B350ull, {0x40, 0x53, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x8B, 0xDA, 0x48, 0x8D, 0x54, 0x24, 0x40, 0xE8, 0x5D, 0x67, 0x00, 0x00, 0x85}},
    {"ChannelSetVolume", 0x00A5B3B0ull, {0x48, 0x83, 0xEC, 0x38, 0x0F, 0x29, 0x74, 0x24, 0x20, 0x48, 0x8D, 0x54, 0x24, 0x50, 0x0F, 0x28, 0xF1, 0xE8, 0xFA, 0x66}},
    {"ChannelStop", 0x00A5B3F0ull, {0x48, 0x83, 0xEC, 0x28, 0x48, 0x8D, 0x54, 0x24, 0x38, 0xE8, 0xC2, 0x66, 0x00, 0x00, 0x85, 0xC0, 0x75, 0x0A, 0x48, 0x8B}},
    {"SoundGetLength", 0x00A5ACD0ull, {0x48, 0x89, 0x5C, 0x24, 0x08, 0x57, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x8B, 0xFA, 0x41, 0x8B, 0xD8, 0x48, 0x8D, 0x54, 0x24}},
    {"SoundRelease", 0x00A5AE00ull, {0x48, 0x83, 0xEC, 0x28, 0x48, 0x8D, 0x54, 0x24, 0x38, 0xE8, 0x52, 0x68, 0x01, 0x00, 0x85, 0xC0, 0x75, 0x0D, 0x48, 0x8B}},
    {"SoundSetDefaults", 0x00A5AEA0ull, {0x48, 0x83, 0xEC, 0x78, 0x0F, 0x29, 0x74, 0x24, 0x60, 0x48, 0x8D, 0x54, 0x24, 0x30, 0x0F, 0x29, 0x7C, 0x24, 0x50, 0x0F}},
    {"DspRelease", 0x00A5BB70ull, {0x48, 0x83, 0xEC, 0x28, 0x48, 0x8D, 0x54, 0x24, 0x38, 0xE8, 0xE2, 0x5A, 0x01, 0x00, 0x85, 0xC0, 0x75, 0x0D, 0x48, 0x8B}},
    {"DspRemove", 0x00A5BBA0ull, {0x48, 0x83, 0xEC, 0x28, 0x48, 0x8D, 0x54, 0x24, 0x38, 0xE8, 0xB2, 0x5A, 0x01, 0x00, 0x85, 0xC0, 0x75, 0x0B, 0x48, 0x8B}},
};

struct Fmod {
    SystemUpdateFn update = nullptr;   // (the trampoline)
    SystemCreateSoundFn createSound = nullptr;
    SystemPlaySoundFn playSound = nullptr;
    SystemCreateDspFn createDsp = nullptr;
    ChannelAddDspFn addDsp = nullptr;
    ChannelGetPositionFn getPosition = nullptr;
    ChannelSetPausedFn setPaused = nullptr;
    ChannelSetSpeakerMixFn setSpeakerMix = nullptr;
    ChannelSetVolumeFn setVolume = nullptr;
    ChannelStopFn stop = nullptr;
    SoundGetLengthFn getLength = nullptr;
    SoundReleaseFn release = nullptr;
    SoundSetDefaultsFn setDefaults = nullptr;
    DspReleaseFn dspRelease = nullptr;
    DspRemoveFn dspRemove = nullptr;
};
Fmod g_fmod;
std::atomic_bool g_available{false};
std::atomic<void*> g_system{nullptr};

// ---------------------------------------------------------------- settings ----------------------------------------------
struct Config {
    float volume = 0.8f;        // 0 .. 1
    float radioEffect = 1.0f;   // 0 .. 1
    bool shuffle = true;
    bool arrowKeys = true;
    bool underwaterMuffle = true;   // (v0.9.586) muffled while Lara is under water
    float underwaterVolume = 1.0f;  // (v0.9.589) the muffled music's level, 0.25 .. 1.5 (0.9.586: 0.8 fixed)
    bool playInCutscenes = true;    // (v0.9.590) off: held silent while a cutscene plays
};
// (v0.9.493) the only radio LaraFM plays from: Lara's own, the item on her marker_PDA
constexpr const char* kLaraRadioObject = "g_walkie_talkie";
std::mutex g_configMutex;
Config g_config;
std::atomic<float> g_volume{0.8f}, g_radioEffect{1.0f};
std::atomic_bool g_shuffle{true}, g_arrowKeys{true};
std::atomic_bool g_underwaterMuffle{true};   // (v0.9.586) the option
std::atomic_bool g_laraUnderwater{false};    // (v0.9.586) under water now (the game thread's tick; v0.9.589: the radio)
std::atomic<float> g_underwaterVolume{1.0f};  // (v0.9.589)
std::atomic_bool g_playInCutscenes{true};     // (v0.9.590) the option
std::atomic_bool g_inCutscene{false};         // (v0.9.590) a cutscene plays (the game thread's tick)

const wchar_t* GameDirW() {
    static wchar_t dir[MAX_PATH]{};
    if (dir[0]) return dir;
    GetModuleFileNameW(nullptr, dir, MAX_PATH);
    wchar_t* slash = dir;
    for (wchar_t* p = dir; *p; ++p)
        if (*p == L'\\' || *p == L'/') slash = p + 1;
    *slash = L'\0';
    return dir;
}

const char* IniPath() {
    static char path[MAX_PATH]{};
    if (path[0]) return path;
    char dir[MAX_PATH]{};
    GetModuleFileNameA(nullptr, dir, MAX_PATH);
    char* slash = dir;
    for (char* p = dir; *p; ++p)
        if (*p == '\\' || *p == '/') slash = p + 1;
    *slash = '\0';
    std::snprintf(path, sizeof(path), "%sROTTRFramework.ini", dir);
    return path;
}

std::wstring FolderPath() { return std::wstring(GameDirW()) + L"ROTTRFramework\\LaraFM"; }

constexpr const char* kSection = "LaraFM";

float ReadIniFloat(const char* key, float fallback) {
    char buf[64]{};
    GetPrivateProfileStringA(kSection, key, "", buf, sizeof(buf), IniPath());
    if (!buf[0]) return fallback;
    char* end = nullptr;
    const float v = std::strtof(buf, &end);
    return end != buf && std::isfinite(v) ? v : fallback;
}

void LoadConfig() {
    Config c;
    c.volume = std::clamp(ReadIniFloat("Volume", c.volume), 0.0f, 1.0f);
    c.radioEffect = std::clamp(ReadIniFloat("RadioEffect", c.radioEffect), 0.0f, 1.0f);
    c.shuffle = GetPrivateProfileIntA(kSection, "Shuffle", 1, IniPath()) != 0;
    c.arrowKeys = GetPrivateProfileIntA(kSection, "ArrowKeys", 1, IniPath()) != 0;
    c.underwaterMuffle = GetPrivateProfileIntA(kSection, "UnderwaterMuffle", 1, IniPath()) != 0;   // (v0.9.586)
    c.underwaterVolume = std::clamp(ReadIniFloat("UnderwaterVolume", c.underwaterVolume), 0.25f, 1.5f);   // (v0.9.589)
    c.playInCutscenes = GetPrivateProfileIntA(kSection, "PlayInCutscenes", 1, IniPath()) != 0;   // (v0.9.590)
    std::lock_guard<std::mutex> lock(g_configMutex);
    g_config = c;
    g_volume.store(c.volume);
    g_radioEffect.store(c.radioEffect);
    g_shuffle.store(c.shuffle);
    g_arrowKeys.store(c.arrowKeys);
    g_underwaterMuffle.store(c.underwaterMuffle);
    g_underwaterVolume.store(c.underwaterVolume);
    g_playInCutscenes.store(c.playInCutscenes);
}

void SaveConfig() {
    Config c;
    {
        std::lock_guard<std::mutex> lock(g_configMutex);
        c = g_config;
    }
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.3f", c.volume);
    WritePrivateProfileStringA(kSection, "Volume", buf, IniPath());
    std::snprintf(buf, sizeof(buf), "%.3f", c.radioEffect);
    WritePrivateProfileStringA(kSection, "RadioEffect", buf, IniPath());
    WritePrivateProfileStringA(kSection, "Shuffle", c.shuffle ? "1" : "0", IniPath());
    WritePrivateProfileStringA(kSection, "ArrowKeys", c.arrowKeys ? "1" : "0", IniPath());
    WritePrivateProfileStringA(kSection, "UnderwaterMuffle", c.underwaterMuffle ? "1" : "0", IniPath());
    std::snprintf(buf, sizeof(buf), "%.3f", c.underwaterVolume);   // (v0.9.589)
    WritePrivateProfileStringA(kSection, "UnderwaterVolume", buf, IniPath());
    WritePrivateProfileStringA(kSection, "PlayInCutscenes", c.playInCutscenes ? "1" : "0", IniPath());   // (v0.9.590)
    // (v0.9.493: 0.9.491 / 492's radio prop keys removed from the ini)
    for (const char* gone : {"RadioProps", "RadioRange", "RadioObjects", "LaraRadioObject"})
        WritePrivateProfileStringA(kSection, gone, nullptr, IniPath());
}

Config CurrentConfig() {
    std::lock_guard<std::mutex> lock(g_configMutex);
    return g_config;
}

std::string Utf8(const std::wstring& w) {
    if (w.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<std::size_t>(std::max(n, 0)), '\0');
    if (n > 0) WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), out.data(), n, nullptr, nullptr);
    return out;
}

// ---------------------------------------------------------------- the tracks --------------------------------------------
struct Track {
    std::wstring path;
    std::string name;   // UTF-8, the file's name without its extension (with its subfolder)
};
std::mutex g_tracksMutex;
std::vector<Track> g_tracks;
std::atomic<std::uint64_t> g_tracksVersion{0};

bool SupportedExtension(std::wstring ext) {
    for (auto& ch : ext) ch = static_cast<wchar_t>(towlower(ch));
    static const wchar_t* const kExts[] = {L".mp3", L".m4a", L".mp4", L".aac", L".wav", L".flac", L".wma", L".ogg"};
    for (const auto* e : kExts)
        if (ext == e) return true;
    return false;
}

int ScanTracks() {
    namespace fs = std::filesystem;
    std::vector<Track> found;
    std::error_code ec;
    const fs::path folder(FolderPath());
    fs::create_directories(folder, ec);
    for (fs::recursive_directory_iterator it(folder, fs::directory_options::skip_permission_denied, ec), end;
         !ec && it != end; it.increment(ec)) {
        if (!it->is_regular_file(ec) || !SupportedExtension(it->path().extension().wstring())) continue;
        Track t;
        t.path = it->path().wstring();
        fs::path rel = it->path().lexically_relative(folder);
        rel.replace_extension();
        t.name = Utf8(rel.wstring());
        found.push_back(std::move(t));
    }
    std::sort(found.begin(), found.end(), [](const Track& a, const Track& b) { return _wcsicmp(a.path.c_str(), b.path.c_str()) < 0; });
    const int count = static_cast<int>(found.size());
    {
        std::lock_guard<std::mutex> lock(g_tracksMutex);
        g_tracks.swap(found);
    }
    g_tracksVersion.fetch_add(1);
    return count;
}

int TrackCount() {
    std::lock_guard<std::mutex> lock(g_tracksMutex);
    return static_cast<int>(g_tracks.size());
}

bool TrackAt(int index, Track& out) {
    std::lock_guard<std::mutex> lock(g_tracksMutex);
    if (index < 0 || index >= static_cast<int>(g_tracks.size())) return false;
    out = g_tracks[static_cast<std::size_t>(index)];
    return true;
}

// ---------------------------------------------------------------- one track's decoding ----------------------------------
// A track's stream: the decoder thread (detached, holding its own reference) fills the ring buffer; FMOD's stream
// thread empties it in PcmRead. Native: FMOD decodes the file itself from data.
struct Stream {
    std::wstring path;
    std::string name;
    int index = -1;
    std::mutex mutex;
    std::condition_variable room;
    std::vector<std::int16_t> ring;   // interleaved stereo
    std::size_t readPos = 0, count = 0;
    int rate = 0;
    std::uint64_t durationMs = 0;
    bool eof = false;                 // (under mutex)
    std::atomic_bool drained{false};  // eof and the ring empty (PcmRead)
    std::atomic_bool ready{false};    // prebuffered, ended, failed or native
    std::atomic_bool failed{false};
    std::atomic_bool cancel{false};
    std::atomic_bool finished{false};  // the decoder thread is done with it
    bool native = false;
    std::vector<std::uint8_t> data;   // native: the whole file
    std::string error;
};

template <typename T>
void SafeRelease(T*& p) {
    if (p) {
        p->Release();
        p = nullptr;
    }
}

bool PushFrames(Stream& s, const std::int16_t* frames, std::size_t samples) {
    std::size_t done = 0;
    std::unique_lock<std::mutex> lock(s.mutex);
    const std::size_t capacity = s.ring.size();
    while (done < samples) {
        s.room.wait_for(lock, std::chrono::milliseconds(20), [&] { return s.cancel.load() || s.count < capacity; });
        if (s.cancel.load()) return false;
        while (done < samples && s.count < capacity) {
            const std::size_t writePos = (s.readPos + s.count) % capacity;
            const std::size_t chunk = std::min({samples - done, capacity - s.count, capacity - writePos});
            std::memcpy(&s.ring[writePos], frames + done, chunk * sizeof(std::int16_t));
            s.count += chunk;
            done += chunk;
        }
        if (!s.ready.load() && s.count >= static_cast<std::size_t>(s.rate)) s.ready.store(true);   // half a second
    }
    return true;
}

std::int16_t Clamp16(int v) { return static_cast<std::int16_t>(std::clamp(v, -32768, 32767)); }

// Windows Media Foundation: false when it cannot open the file at all (FMOD's own decoders are tried then).
bool DecodeWithMediaFoundation(Stream& s) {
    IMFSourceReader* reader = nullptr;
    if (FAILED(MFCreateSourceReaderFromURL(s.path.c_str(), nullptr, &reader)) || !reader) return false;
    reader->SetStreamSelection(static_cast<DWORD>(MF_SOURCE_READER_ALL_STREAMS), FALSE);
    if (FAILED(reader->SetStreamSelection(static_cast<DWORD>(MF_SOURCE_READER_FIRST_AUDIO_STREAM), TRUE))) {
        SafeRelease(reader);
        return false;
    }
    IMFMediaType* wanted = nullptr;
    HRESULT hr = MFCreateMediaType(&wanted);
    if (SUCCEEDED(hr)) {
        wanted->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
        wanted->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_PCM);
        wanted->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
        hr = reader->SetCurrentMediaType(static_cast<DWORD>(MF_SOURCE_READER_FIRST_AUDIO_STREAM), nullptr, wanted);
    }
    SafeRelease(wanted);
    IMFMediaType* actual = nullptr;
    if (SUCCEEDED(hr)) hr = reader->GetCurrentMediaType(static_cast<DWORD>(MF_SOURCE_READER_FIRST_AUDIO_STREAM), &actual);
    UINT32 channels = 0, rate = 0, bits = 0;
    if (SUCCEEDED(hr) && actual) {
        channels = MFGetAttributeUINT32(actual, MF_MT_AUDIO_NUM_CHANNELS, 0);
        rate = MFGetAttributeUINT32(actual, MF_MT_AUDIO_SAMPLES_PER_SECOND, 0);
        bits = MFGetAttributeUINT32(actual, MF_MT_AUDIO_BITS_PER_SAMPLE, 0);
    }
    SafeRelease(actual);
    if (FAILED(hr) || channels == 0 || channels > 8 || rate < 8000 || rate > 192000 || bits != 16) {
        SafeRelease(reader);
        return false;
    }
    PROPVARIANT duration;
    PropVariantInit(&duration);
    if (SUCCEEDED(reader->GetPresentationAttribute(static_cast<DWORD>(MF_SOURCE_READER_MEDIASOURCE), MF_PD_DURATION, &duration)) &&
        duration.vt == VT_UI8)
        s.durationMs = duration.uhVal.QuadPart / 10000ull;
    PropVariantClear(&duration);
    {
        std::lock_guard<std::mutex> lock(s.mutex);
        s.rate = static_cast<int>(rate);
        s.ring.assign(static_cast<std::size_t>(rate) * 2u * 2u, 0);   // two seconds of stereo
    }
    std::vector<std::int16_t> stereo;
    while (!s.cancel.load()) {
        DWORD flags = 0;
        LONGLONG timestamp = 0;
        IMFSample* sample = nullptr;
        hr = reader->ReadSample(static_cast<DWORD>(MF_SOURCE_READER_FIRST_AUDIO_STREAM), 0, nullptr, &flags, &timestamp, &sample);
        if (FAILED(hr) || (flags & (MF_SOURCE_READERF_ENDOFSTREAM | MF_SOURCE_READERF_ERROR))) {
            SafeRelease(sample);
            break;
        }
        if (!sample) continue;
        IMFMediaBuffer* buffer = nullptr;
        if (SUCCEEDED(sample->ConvertToContiguousBuffer(&buffer)) && buffer) {
            BYTE* bytes = nullptr;
            DWORD length = 0;
            if (SUCCEEDED(buffer->Lock(&bytes, nullptr, &length)) && bytes) {
                const auto* in = reinterpret_cast<const std::int16_t*>(bytes);
                const std::size_t frames = length / (2u * channels);
                stereo.resize(frames * 2);
                for (std::size_t f = 0; f < frames; ++f) {
                    const std::int16_t* x = in + f * channels;
                    int l = x[0], r = channels > 1 ? x[1] : x[0];
                    if (channels >= 3) {   // (FL FR FC LFE BL BR ...: the centre and the backs folded in)
                        const int c = x[2], bl = channels >= 6 ? x[4] : 0, br = channels >= 6 ? x[5] : 0;
                        l = static_cast<int>((l + 0.707f * c + 0.707f * bl) * 0.6f);
                        r = static_cast<int>((r + 0.707f * c + 0.707f * br) * 0.6f);
                    }
                    stereo[f * 2] = Clamp16(l);
                    stereo[f * 2 + 1] = Clamp16(r);
                }
                buffer->Unlock();
                buffer->Release();
                sample->Release();
                if (!PushFrames(s, stereo.data(), stereo.size())) break;
                continue;
            }
            buffer->Release();
        }
        sample->Release();
    }
    SafeRelease(reader);
    {
        std::lock_guard<std::mutex> lock(s.mutex);
        s.eof = true;
        if (s.count == 0) s.drained.store(true);
    }
    s.ready.store(true);
    return true;
}

bool ReadWholeFile(const std::wstring& path, std::vector<std::uint8_t>& out) {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER size{};
    bool ok = GetFileSizeEx(file, &size) && size.QuadPart > 0 && size.QuadPart < (512ll << 20);
    if (ok) {
        out.resize(static_cast<std::size_t>(size.QuadPart));
        DWORD read = 0;
        ok = ReadFile(file, out.data(), static_cast<DWORD>(out.size()), &read, nullptr) && read == out.size();
    }
    CloseHandle(file);
    return ok;
}

void DecoderMain(std::shared_ptr<Stream> s) {
    const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const HRESULT mf = MFStartup(MF_VERSION, MFSTARTUP_LITE);
    bool opened = false;
    if (SUCCEEDED(mf)) opened = DecodeWithMediaFoundation(*s);
    if (!opened && !s->cancel.load()) {
        if (ReadWholeFile(s->path, s->data)) {
            s->native = true;
        } else {
            s->error = "the file could not be read";
            s->failed.store(true);
        }
        s->ready.store(true);
    }
    if (SUCCEEDED(mf)) MFShutdown();
    if (SUCCEEDED(com)) CoUninitialize();
    s->finished.store(true);
}

std::shared_ptr<Stream> StartDecoder(const Track& track, int index) {
    auto s = std::make_shared<Stream>();
    s->path = track.path;
    s->name = track.name;
    s->index = index;
    try {
        std::thread(DecoderMain, s).detach();
    } catch (...) {
        s->error = "no decoder thread";
        s->failed.store(true);
        s->ready.store(true);
        s->finished.store(true);
    }
    return s;
}

void StopDecoder(const std::shared_ptr<Stream>& s) {
    if (!s) return;
    s->cancel.store(true);
    s->room.notify_all();
    for (int i = 0; i < 200 && !s->finished.load(); ++i) Sleep(5);   // (ReadSample returns within milliseconds)
}

// ---------------------------------------------------------------- FMOD's callbacks ---------------------------------------
std::mutex g_pcmMutex;
std::shared_ptr<Stream> g_pcmStream;   // the stream PcmRead feeds FMOD from

void SetPcmStream(std::shared_ptr<Stream> s) {
    std::lock_guard<std::mutex> lock(g_pcmMutex);
    g_pcmStream = std::move(s);
}

FmodResult __stdcall PcmRead(void* /*sound*/, void* data, unsigned int datalen) {
    auto* out = static_cast<std::int16_t*>(data);
    const std::size_t want = datalen / sizeof(std::int16_t);
    std::shared_ptr<Stream> s;
    {
        std::lock_guard<std::mutex> lock(g_pcmMutex);
        s = g_pcmStream;
    }
    std::size_t got = 0;
    if (s) {
        {
            std::lock_guard<std::mutex> lock(s->mutex);
            const std::size_t capacity = s->ring.size();
            while (got < want && s->count > 0 && capacity) {
                const std::size_t chunk = std::min({want - got, s->count, capacity - s->readPos});
                std::memcpy(out + got, &s->ring[s->readPos], chunk * sizeof(std::int16_t));
                s->readPos = (s->readPos + chunk) % capacity;
                s->count -= chunk;
                got += chunk;
            }
            if (s->eof && s->count == 0) s->drained.store(true);
        }
        s->room.notify_one();
    }
    if (got < want) std::memset(out + got, 0, (want - got) * sizeof(std::int16_t));
    return kFmodOk;
}

FmodResult __stdcall PcmSetPos(void* /*sound*/, int /*subsound*/, unsigned int /*position*/, unsigned int /*unit*/) {
    return kFmodOk;   // (no seeking: the stream goes on where it is)
}

// The radio: one mono band of the music (two high-passes at 320 Hz, two low-passes at 3400 Hz, +5 dB at 1800 Hz), a soft
// clip and a little hiss, mixed with the music by the effect's strength. (At 48 kHz, the usual FMOD mix rate.)
struct Biquad {
    float b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0, z1 = 0, z2 = 0;
    float Run(float x) {
        const float y = b0 * x + z1;
        z1 = b1 * x - a1 * y + z2;
        z2 = b2 * x - a2 * y;
        return y;
    }
};
Biquad MakeFilter(int type, float freq, float q, float gainDb = 0.0f) {   // 0 low-pass, 1 high-pass, 2 peak
    constexpr float kRate = 48000.0f, kPi = 3.14159265f;
    const float w = 2.0f * kPi * freq / kRate, cw = std::cos(w), alpha = std::sin(w) / (2.0f * q);
    float b0, b1, b2, a0, a1, a2;
    if (type == 0) {
        b0 = (1 - cw) / 2; b1 = 1 - cw; b2 = (1 - cw) / 2; a0 = 1 + alpha; a1 = -2 * cw; a2 = 1 - alpha;
    } else if (type == 1) {
        b0 = (1 + cw) / 2; b1 = -(1 + cw); b2 = (1 + cw) / 2; a0 = 1 + alpha; a1 = -2 * cw; a2 = 1 - alpha;
    } else {
        const float a = std::pow(10.0f, gainDb / 40.0f);
        b0 = 1 + alpha * a; b1 = -2 * cw; b2 = 1 - alpha * a; a0 = 1 + alpha / a; a1 = -2 * cw; a2 = 1 - alpha / a;
    }
    Biquad f;
    f.b0 = b0 / a0; f.b1 = b1 / a0; f.b2 = b2 / a0; f.a1 = a1 / a0; f.a2 = a2 / a0;
    return f;
}
struct RadioEffect {
    Biquad hp1 = MakeFilter(1, 320.0f, 0.707f), hp2 = MakeFilter(1, 320.0f, 0.707f);
    Biquad lp1 = MakeFilter(0, 3400.0f, 0.707f), lp2 = MakeFilter(0, 3400.0f, 0.707f);
    Biquad mid = MakeFilter(2, 1800.0f, 1.0f, 5.0f);
    std::uint32_t noise = 0x1234567u;
    float Next(float mono) {
        noise ^= noise << 13; noise ^= noise >> 17; noise ^= noise << 5;
        const float hiss = (static_cast<float>(noise & 0xFFFF) / 32768.0f - 1.0f) * 0.012f;
        float x = hp2.Run(hp1.Run(mono + hiss));
        x = mid.Run(lp2.Run(lp1.Run(x)));
        constexpr float kDrive = 2.2f;
        static const float kNorm = 1.0f / std::tanh(kDrive);
        return std::tanh(x * kDrive) * kNorm * 0.95f;
    }
};
RadioEffect g_effect;   // (FMOD's mixer thread only)

// (v0.9.586) Under water: the music (after the radio) through a low-pass of its own per channel (two at 420 Hz: 24 dB an
// octave, the treble and most of the mids gone, a little quieter), blended in and out over about 0.15 s as her head goes
// under and comes up. (FMOD's mixer thread only.) (v0.9.589) At 550 Hz, its level "Under water volume" (1: as loud as
// the clear music's low part; 0.9.586's 0.8 took too much), and under water is the radio's own height under the water's
// surface (TickGameThread).
struct Muffle {
    static constexpr int kChannels = 8;
    Biquad lp1[kChannels], lp2[kChannels];
    float mix = 0.0f;   // 0 clear .. 1 muffled
    Muffle() {
        for (int c = 0; c < kChannels; ++c) lp1[c] = lp2[c] = MakeFilter(0, 550.0f, 0.707f);
    }
};
Muffle g_muffle;

FmodResult __stdcall DspRead(void* /*state*/, float* in, float* out, unsigned int length, int inChannels, int outChannels) {
    if (!in || !out) return kFmodOk;
    const float amount = std::clamp(g_radioEffect.load(std::memory_order_relaxed), 0.0f, 1.0f);
    const bool muffleOn = g_underwaterMuffle.load(std::memory_order_relaxed);
    const float muffleTarget = muffleOn && g_laraUnderwater.load(std::memory_order_relaxed) ? 1.0f : 0.0f;
    if (!muffleOn) g_muffle.mix = 0.0f;
    const bool radio = amount > 0.001f;
    if (inChannels != outChannels || inChannels <= 0 || (!radio && !muffleOn)) {
        const int ch = std::min(inChannels, outChannels);
        for (unsigned i = 0; i < length; ++i)
            for (int c = 0; c < outChannels; ++c) out[i * outChannels + c] = c < ch ? in[i * inChannels + c] : 0.0f;
        return kFmodOk;
    }
    const float dry = 1.0f - amount, inv = 1.0f / static_cast<float>(inChannels);
    constexpr float kMuffleStep = 1.0f / (0.15f * 48000.0f);   // (a sample's share of the 0.15 s blend)
    const float kMuffleGain = std::clamp(g_underwaterVolume.load(std::memory_order_relaxed), 0.25f, 1.5f);
    for (unsigned i = 0; i < length; ++i) {
        const float* x = in + static_cast<std::size_t>(i) * inChannels;
        float* y = out + static_cast<std::size_t>(i) * outChannels;
        if (radio) {
            float mono = 0.0f;
            for (int c = 0; c < inChannels; ++c) mono += x[c];
            const float wet = g_effect.Next(mono * inv) * amount;
            for (int c = 0; c < outChannels; ++c) y[c] = x[c] * dry + wet;
        } else {
            for (int c = 0; c < outChannels; ++c) y[c] = x[c];
        }
        // (the low-passes run all the while the option is on, so the muffle starts from the music's state: no click)
        float& mix = g_muffle.mix;
        if (mix < muffleTarget) mix = std::min(muffleTarget, mix + kMuffleStep);
        else if (mix > muffleTarget) mix = std::max(muffleTarget, mix - kMuffleStep);
        if (!muffleOn) continue;
        for (int c = 0; c < outChannels && c < Muffle::kChannels; ++c) {
            const float low = g_muffle.lp2[c].Run(g_muffle.lp1[c].Run(y[c]));
            if (mix > 0.0f) y[c] += (low * kMuffleGain - y[c]) * mix;
        }
    }
    return kFmodOk;
}

// ---------------------------------------------------------------- the player (FMOD's update thread) ---------------------
enum class Command { PlayIndex, Up, Play, Pause, TogglePause, Stop, Next, Prev };
struct Request {
    Command command;
    int index;
};
std::mutex g_requestMutex;
std::vector<Request> g_requests;

void Push(Command command, int index = -1) {
    std::lock_guard<std::mutex> lock(g_requestMutex);
    if (g_requests.size() < 32) g_requests.push_back({command, index});
}

enum class State { Stopped, Loading, Playing, Paused };
struct Status {
    State state = State::Stopped;
    int index = -1;
    std::string track;
    unsigned positionMs = 0, lengthMs = 0;
    std::string error;
    const char* hold = nullptr;   // (v0.9.493) why it is held silent for now (the game loads, a menu, no Lara)
};
std::mutex g_statusMutex;
Status g_status;
std::atomic_bool g_active{false};   // playing, paused or loading (the game thread places the source then)

struct Playing {
    std::shared_ptr<Stream> stream;
    void* sound = nullptr;
    void* channel = nullptr;
    bool channelPaused = false;
    ULONGLONG drainedAt = 0;
};
Playing g_now;                       // (FMOD thread)
std::shared_ptr<Stream> g_pending;   // (FMOD thread) decoding its first half second
bool g_userPaused = false;
void* g_dsp = nullptr;
std::vector<int> g_history;
int g_failures = 0;
std::mt19937 g_random{static_cast<unsigned>(GetTickCount64())};

// From the game thread: the source's place as heard from the camera, the game's pause.
std::atomic<float> g_mixGain{1.0f}, g_mixPan{0.0f};
std::atomic_bool g_menuPaused{false};
std::atomic_bool g_noLara{false};   // (v0.9.493) no Lara for a moment (the main menu, a load): no radio to play from
// (v0.9.495) her radio, g_walkie_talkie, in her inventory: 1 yes, 0 no, -1 not known (no Lara); and its Instance there
std::atomic<int> g_hasRadio{-1};
std::atomic_bool g_radioHere{false};
std::atomic_bool g_giveRadio{false};   // the tab's "Give Lara her radio" (done on the game thread)
constexpr const char* kNoRadio = "No radio - can't listen to music!";

void SetError(const std::string& error) {
    std::lock_guard<std::mutex> lock(g_statusMutex);
    g_status.error = error;
}

void ReleaseCurrent() {
    if (g_now.channel) {
        if (g_dsp) g_fmod.dspRemove(g_dsp);
        g_fmod.stop(g_now.channel);
    }
    if (g_now.sound) g_fmod.release(g_now.sound);   // (FMOD's stream thread stops reading it first)
    SetPcmStream(nullptr);
    StopDecoder(g_now.stream);
    g_now = {};
}

void CancelPending() {
    StopDecoder(g_pending);
    g_pending.reset();
}

int RandomIndex(int count, int avoid) {
    if (count <= 1) return 0;
    std::uniform_int_distribution<int> pick(0, count - 2);
    int i = pick(g_random);
    if (avoid >= 0 && i >= avoid) ++i;
    return i;
}

int CurrentIndex() {
    if (g_pending) return g_pending->index;
    if (g_now.stream) return g_now.stream->index;
    return -1;
}

bool StartTrack(int index, bool remember) {
    Track track;
    if (TrackCount() == 0) ScanTracks();
    if (!TrackAt(index, track)) return false;
    ReleaseCurrent();
    CancelPending();
    g_pending = StartDecoder(track, index);
    g_userPaused = false;
    if (remember) {
        g_history.push_back(index);
        if (g_history.size() > 200) g_history.erase(g_history.begin());
    }
    {
        std::lock_guard<std::mutex> lock(g_statusMutex);
        g_status.state = State::Loading;
        g_status.index = index;
        g_status.track = track.name;
        g_status.positionMs = 0;
        g_status.lengthMs = 0;
        g_status.error.clear();
    }
    g_active.store(true);
    return true;
}

void StopAll() {
    ReleaseCurrent();
    CancelPending();
    g_userPaused = false;
    g_active.store(false);
    std::lock_guard<std::mutex> lock(g_statusMutex);
    g_status.state = State::Stopped;
    g_status.positionMs = 0;
}

void PlayNext() {
    if (TrackCount() == 0) ScanTracks();
    const int count = TrackCount();
    if (count == 0) {
        StopAll();
        SetError("No music in ROTTRFramework\\LaraFM");
        return;
    }
    const int current = CurrentIndex();
    const int next = g_shuffle.load() ? RandomIndex(count, current) : (current + 1) % count;
    StartTrack(next, true);
}

void PlayPrevious() {
    const int count = TrackCount();
    if (count == 0) return;
    if (g_shuffle.load() && g_history.size() >= 2) {
        g_history.pop_back();
        StartTrack(g_history.back(), false);
        return;
    }
    const int current = CurrentIndex();
    StartTrack(current < 0 ? count - 1 : (current - 1 + count) % count, true);
}

void PlayRandom() {
    if (TrackCount() == 0) ScanTracks();
    const int count = TrackCount();
    if (count == 0) {
        SetError("No music in ROTTRFramework\\LaraFM");
        return;
    }
    StartTrack(RandomIndex(count, CurrentIndex()), true);
}

bool HasTrack() { return g_now.channel != nullptr || g_pending != nullptr; }

void SetUserPaused(bool paused) {
    if (!g_now.channel && !g_pending) return;
    g_userPaused = paused;
}

void ProcessRequests() {
    std::vector<Request> requests;
    {
        std::lock_guard<std::mutex> lock(g_requestMutex);
        requests.swap(g_requests);
    }
    for (const auto& r : requests) {
        // (v0.9.495) without her radio nothing starts (pausing and stopping still do)
        const bool starts = r.command != Command::Pause && r.command != Command::TogglePause && r.command != Command::Stop;
        if (starts && g_hasRadio.load() == 0) continue;
        switch (r.command) {
        case Command::PlayIndex: StartTrack(r.index, true); break;
        case Command::Up:   // (the Up key: a paused track resumes; with nothing playing, a random one)
            if (HasTrack()) {
                if (g_userPaused) SetUserPaused(false);
            } else {
                PlayRandom();
            }
            break;
        case Command::Play:
            if (HasTrack()) {
                SetUserPaused(false);
            } else if (r.index >= 0) {
                StartTrack(r.index, true);
            } else if (g_shuffle.load()) {
                PlayRandom();
            } else {
                StartTrack(0, true);
            }
            break;
        case Command::Pause: SetUserPaused(true); break;
        case Command::TogglePause: SetUserPaused(!g_userPaused); break;
        case Command::Stop: StopAll(); break;
        case Command::Next: PlayNext(); break;
        case Command::Prev: PlayPrevious(); break;
        }
    }
}

void EnsureDsp(void* system) {
    if (g_dsp) return;
    static FmodDspDescription description{};
    strncpy_s(description.name, "LaraFM radio", _TRUNCATE);
    description.version = 0x00010000;
    description.channels = 0;
    description.read = reinterpret_cast<void*>(&DspRead);
    const FmodResult r = g_fmod.createDsp(system, &description, &g_dsp);
    if (r != kFmodOk) {
        g_dsp = nullptr;
        Log::Writef("LaraFM: FMOD's createDSP failed (%d); the music plays without the radio effect", r);
    }
}

void ApplyMix() {
    if (!g_now.channel) return;
    const float gain = std::clamp(g_mixGain.load(std::memory_order_relaxed), 0.0f, 1.0f);
    const float pan = std::clamp(g_mixPan.load(std::memory_order_relaxed), -1.0f, 1.0f);
    g_fmod.setVolume(g_now.channel, std::clamp(g_volume.load(std::memory_order_relaxed), 0.0f, 1.0f) * gain);
    // FL FR C LFE BL BR SL SR: a balance between the two front speakers
    const float levels[8] = {std::min(1.0f, 1.0f - pan), std::min(1.0f, 1.0f + pan), 0, 0, 0, 0, 0, 0};
    g_fmod.setSpeakerMix(g_now.channel, levels);
}

bool OpenPending(void* system) {
    auto s = g_pending;
    FmodCreateSoundExInfo ex{};
    ex.cbsize = sizeof(ex);
    void* sound = nullptr;
    FmodResult r;
    if (s->native) {
        ex.length = static_cast<unsigned>(s->data.size());
        r = g_fmod.createSound(system, reinterpret_cast<const char*>(s->data.data()),
                               kModeLoopOff | kMode2D | kModeSoftware | kModeCreateStream | kModeOpenMemoryPoint, &ex, &sound);
    } else {
        ex.length = 0xFFFFFFFCu;   // (ended by the decoder, not by this)
        ex.numchannels = 2;
        ex.defaultfrequency = s->rate;
        ex.format = kFormatPcm16;
        ex.decodebuffersize = 2048;
        ex.pcmreadcallback = reinterpret_cast<void*>(&PcmRead);
        ex.pcmsetposcallback = reinterpret_cast<void*>(&PcmSetPos);
        SetPcmStream(s);
        r = g_fmod.createSound(system, nullptr, kModeLoopOff | kMode2D | kModeSoftware | kModeCreateStream | kModeOpenUser,
                               &ex, &sound);
    }
    if (r != kFmodOk || !sound) {
        SetPcmStream(nullptr);
        s->error = "FMOD could not open it (" + std::to_string(r) + ")";
        return false;
    }
    if (!s->native) g_fmod.setDefaults(sound, static_cast<float>(s->rate), 1.0f, 0.0f, 0);   // priority 0: never stolen
    void* channel = nullptr;
    r = g_fmod.playSound(system, kChannelFree, sound, true, &channel);
    if (r != kFmodOk || !channel) {
        g_fmod.release(sound);
        SetPcmStream(nullptr);
        s->error = "FMOD could not play it (" + std::to_string(r) + ")";
        return false;
    }
    EnsureDsp(system);
    if (g_dsp) {
        void* connection = nullptr;
        g_fmod.dspRemove(g_dsp);
        const FmodResult d = g_fmod.addDsp(channel, g_dsp, &connection);
        static int s_logs = 0;
        if (d != kFmodOk && s_logs++ < 3) Log::Writef("LaraFM: the radio effect could not be added (%d)", d);
    }
    unsigned lengthMs = static_cast<unsigned>(s->durationMs);
    if (s->native) g_fmod.getLength(sound, &lengthMs, kTimeUnitMs);
    g_now.stream = s;
    g_now.sound = sound;
    g_now.channel = channel;
    g_now.channelPaused = true;
    g_now.drainedAt = 0;
    g_pending.reset();
    ApplyMix();
    {
        std::lock_guard<std::mutex> lock(g_statusMutex);
        g_status.lengthMs = lengthMs;
    }
    Log::Detailf("LaraFM: playing \"%s\" (%s%s)", s->name.c_str(),
                 s->native ? "FMOD's decoder" : "Windows Media Foundation",
                 s->native ? "" : (", " + std::to_string(s->rate) + " Hz").c_str());
    return true;
}

void UpdatePlayer(void* system) {
    // (v0.9.494) A load of the game's stops the music for good (the user's choice over 0.9.493's pause): Up or Play
    // starts it again once in play. Requests that come while it loads are dropped. Read here: the game thread may not
    // tick then.
    if (Features::LoadingProfilerSessionActive()) {
        {
            std::lock_guard<std::mutex> lock(g_requestMutex);
            g_requests.clear();
        }
        if (g_now.channel || g_pending) {
            StopAll();
            Log::Detail("LaraFM: stopped: the game is loading");
        }
        return;
    }
    ProcessRequests();
    if (g_pending && g_pending->ready.load()) {
        if (g_pending->failed.load() || !OpenPending(system)) {
            const std::string name = g_pending->name, error = g_pending->error;
            Log::Writef("LaraFM: \"%s\" could not be played: %s", name.c_str(), error.c_str());
            SetError("\"" + name + "\": " + error);
            CancelPending();
            if (++g_failures < std::min(TrackCount(), 5)) PlayNext();
            else StopAll();
        } else {
            g_failures = 0;
        }
    }
    if (!g_now.channel) {
        if (!g_pending && g_active.load()) g_active.store(false);
        return;
    }
    unsigned positionMs = 0;
    const FmodResult r = g_fmod.getPosition(g_now.channel, &positionMs, kTimeUnitMs);
    bool ended = false;
    if (r != kFmodOk) {
        if (g_now.stream->native || g_now.stream->drained.load()) {
            ended = true;
        } else {
            // (the channel was taken: the same stream on a new one, going on where the decoder is)
            void* channel = nullptr;
            if (g_fmod.playSound(system, kChannelFree, g_now.sound, true, &channel) == kFmodOk && channel) {
                g_now.channel = channel;
                g_now.channelPaused = true;
                if (g_dsp) {
                    void* connection = nullptr;
                    g_fmod.dspRemove(g_dsp);
                    g_fmod.addDsp(channel, g_dsp, &connection);
                }
                Log::Detail("LaraFM: its channel was taken; it goes on on another");
            } else {
                ended = true;
            }
        }
    } else if (!g_now.stream->native && g_now.stream->drained.load()) {
        // (what FMOD still holds plays out first)
        const ULONGLONG now = GetTickCount64();
        if (!g_now.drainedAt) g_now.drainedAt = now;
        else if (now - g_now.drainedAt > 1200) ended = true;
    }
    if (ended) {
        PlayNext();
        return;
    }
    // (v0.9.493) silent while there is no Lara, so no radio of hers, and while the game is paused; it goes on where it
    // was after (a load stops it: above)
    const bool menu = g_menuPaused.load(std::memory_order_relaxed);
    const bool noLara = g_noLara.load(std::memory_order_relaxed);
    const int hasRadio = g_hasRadio.load(std::memory_order_relaxed);
    // (v0.9.590) and, with "Play during cutscenes" off, while a cutscene plays (Features::CutscenePlaying)
    const bool cutscene = g_inCutscene.load(std::memory_order_relaxed) && !g_playInCutscenes.load(std::memory_order_relaxed);
    const char* hold = noLara                     ? "Lara is not in the game"
                       : hasRadio == 0             ? kNoRadio
                       : !g_radioHere.load()       ? "her radio is not here yet"
                       : menu                      ? "the game is paused"
                       : cutscene                  ? "a cutscene plays"
                                                   : nullptr;
    const bool pause = g_userPaused || hold != nullptr;
    if (pause != g_now.channelPaused) {
        g_fmod.setPaused(g_now.channel, pause);
        g_now.channelPaused = pause;
    }
    ApplyMix();
    std::lock_guard<std::mutex> lock(g_statusMutex);
    g_status.state = g_userPaused ? State::Paused : State::Playing;
    g_status.positionMs = positionMs;
    g_status.hold = g_userPaused ? nullptr : hold;
}

FmodResult __fastcall HookSystemUpdate(void* system) {
    void* expected = nullptr;
    if (g_system.compare_exchange_strong(expected, system))
        Log::Detailf("LaraFM: the game's FMOD system %p", system);
    if (system == g_system.load(std::memory_order_relaxed) && g_available.load(std::memory_order_relaxed)) {
        try {
            UpdatePlayer(system);
        } catch (...) {
            static bool s_logged = false;
            if (!s_logged) Log::Write("LaraFM: the player failed (an exception); stopped");
            s_logged = true;
            g_now = {};
            g_pending.reset();
            g_active.store(false);
        }
    }
    return g_fmod.update(system);
}

// ---------------------------------------------------------------- the radio's place (the game thread) ------------------
// (v0.9.495) Only her radio: the g_walkie_talkie item of her inventory (Features::PlayerInventoryObject), placed by its
// own Instance (the one on her marker_PDA, or in her hand). Without it in her inventory, or with no Instance of it in
// the world yet, it is silent; never from Lara herself.
float g_sourcePos[3]{};
float g_sourceOffset[3]{};    // (v0.9.492) what is left of the way from the place before: it follows a moving one
void* g_sourceKey = nullptr;  // the radio Instance followed
bool g_sourceValid = false;
ULONGLONG g_lastTickMs = 0, g_laraSeenMs = 0, g_radioCheckMs = 0;
int g_radioLogged = -2;
std::mutex g_sourceMutex;
std::string g_sourceText = "-";
std::string g_giveText;   // (under g_sourceMutex)

float Distance(const float a[3], const float b[3]) {
    const float dx = a[0] - b[0], dy = a[1] - b[1], dz = a[2] - b[2];
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}
}  // namespace

// ---------------------------------------------------------------- the public side -------------------------------------
bool Install() {
    LoadConfig();
    const int tracks = ScanTracks();
    if (!GameBuild::IsSupportedRetail()) {
        Log::Write("LaraFM: unavailable (not the supported game build)");
        return false;
    }
    for (const auto& f : kFunctions) {
        if (!GameBuild::VerifyBytes(f.rva, f.bytes, sizeof(f.bytes))) {
            Log::Writef("LaraFM: FMOD's %s does not match this build; unavailable", f.name);
            return false;
        }
    }
    const std::uintptr_t base = GameBuild::Base();
    auto at = [base](std::size_t i) { return reinterpret_cast<void*>(base + kFunctions[i].rva); };
    g_fmod.createSound = reinterpret_cast<SystemCreateSoundFn>(at(1));
    g_fmod.playSound = reinterpret_cast<SystemPlaySoundFn>(at(2));
    g_fmod.createDsp = reinterpret_cast<SystemCreateDspFn>(at(3));
    g_fmod.addDsp = reinterpret_cast<ChannelAddDspFn>(at(4));
    g_fmod.getPosition = reinterpret_cast<ChannelGetPositionFn>(at(5));
    g_fmod.setPaused = reinterpret_cast<ChannelSetPausedFn>(at(6));
    g_fmod.setSpeakerMix = reinterpret_cast<ChannelSetSpeakerMixFn>(at(7));
    g_fmod.setVolume = reinterpret_cast<ChannelSetVolumeFn>(at(8));
    g_fmod.stop = reinterpret_cast<ChannelStopFn>(at(9));
    g_fmod.getLength = reinterpret_cast<SoundGetLengthFn>(at(10));
    g_fmod.release = reinterpret_cast<SoundReleaseFn>(at(11));
    g_fmod.setDefaults = reinterpret_cast<SoundSetDefaultsFn>(at(12));
    g_fmod.dspRelease = reinterpret_cast<DspReleaseFn>(at(13));
    g_fmod.dspRemove = reinterpret_cast<DspRemoveFn>(at(14));
    if (MH_CreateHook(at(0), reinterpret_cast<void*>(&HookSystemUpdate), reinterpret_cast<void**>(&g_fmod.update)) != MH_OK) {
        Log::Write("LaraFM: the hook on FMOD's System::update could not be created; unavailable");
        return false;
    }
    g_available.store(true);
    Log::Writef("LaraFM: ready, %d track(s) in ROTTRFramework\\LaraFM", tracks);
    return true;
}

bool Available() { return g_available.load(); }

void TickGameThread() {
    if (!g_available.load(std::memory_order_relaxed)) return;
    g_menuPaused.store(Features::MenuPaused(), std::memory_order_relaxed);
    {   // (v0.9.590)
        const bool cutscene = Features::CutscenePlaying();
        if (cutscene != g_inCutscene.exchange(cutscene, std::memory_order_relaxed) && g_active.load(std::memory_order_relaxed) &&
            !g_playInCutscenes.load(std::memory_order_relaxed)) {
            static int s_logs = 0;
            if (s_logs++ < 8) Log::Detailf("LaraFM: %s", cutscene ? "a cutscene plays: held silent" : "the cutscene is over: on again");
        }
    }
    const ULONGLONG now = GetTickCount64();
    const bool active = g_active.load(std::memory_order_relaxed);
    const bool give = g_giveRadio.exchange(false);
    // her radio: every tick while playing, four times a second otherwise (the tab's message, the keys)
    void* radio = nullptr;
    int hasRadio = g_hasRadio.load(std::memory_order_relaxed);
    if (active || give || now - g_radioCheckMs >= 250) {
        g_radioCheckMs = now;
        hasRadio = Features::PlayerInventoryObject(kLaraRadioObject, &radio);
        g_hasRadio.store(hasRadio, std::memory_order_relaxed);
        g_radioHere.store(radio != nullptr, std::memory_order_relaxed);
        if (hasRadio != g_radioLogged && hasRadio >= 0) {
            Log::Detailf("LaraFM: Lara %s her radio (%s)", hasRadio ? "has" : "does not have", kLaraRadioObject);
            g_radioLogged = hasRadio;
        }
    }
    if (give) {
        std::string text;
        if (hasRadio == 1) text = "She has her radio already.";
        else if (hasRadio < 0) text = "Lara is not in the game (or her radio is not known yet).";
        else if (Features::GivePlayerInventoryObject(kLaraRadioObject)) text = "Given: it comes once the game has loaded it.";
        else text = "The game did not give it (see ROTTRFramework.log).";
        std::lock_guard<std::mutex> lock(g_sourceMutex);
        g_giveText = text;
    }
    if (!active) {
        g_sourceValid = false;
        return;
    }
    const float dt = g_lastTickMs ? std::clamp(static_cast<float>(now - g_lastTickMs) / 1000.0f, 0.0f, 0.25f) : 0.0f;
    g_lastTickMs = now;

    Features::PlayerTransform lara{};
    const bool haveLara = Features::GetPlayerTransform(lara);
    // (no Lara for more than a third of a second: held silent; a respawn's moment without her is not worth a gap)
    if (haveLara) g_laraSeenMs = now;
    g_noLara.store(!haveLara && (!g_laraSeenMs || now - g_laraSeenMs > 350), std::memory_order_relaxed);
    float target[3]{};
    bool radioPlaced = false;   // (v0.9.589) target is the radio's place this tick
    std::string text = hasRadio == 0 ? "no radio" : "-";
    if (radio && Features::InstancePositionNow(radio, target)) {
        radioPlaced = true;
        text = std::string("Lara's radio (") + kLaraRadioObject + ")";
        // Followed exactly (it moves with her); a new radio Instance (a respawn, an outfit) glides over a quarter second.
        if (!g_sourceValid) {
            std::memset(g_sourceOffset, 0, sizeof(g_sourceOffset));
        } else if (radio != g_sourceKey) {
            for (int c = 0; c < 3; ++c) g_sourceOffset[c] = g_sourcePos[c] - target[c];
        } else {
            const float keep = 1.0f - std::clamp(dt / 0.25f, 0.0f, 1.0f);
            for (float& o : g_sourceOffset) o *= keep;
            if (std::fabs(g_sourceOffset[0]) + std::fabs(g_sourceOffset[1]) + std::fabs(g_sourceOffset[2]) < 1.0f)
                std::memset(g_sourceOffset, 0, sizeof(g_sourceOffset));
        }
        for (int c = 0; c < 3; ++c) g_sourcePos[c] = target[c] + g_sourceOffset[c];
        g_sourceKey = radio;
        g_sourceValid = true;
    } else {
        g_radioHere.store(false, std::memory_order_relaxed);   // (held silent: never from anywhere else)
    }
    // (v0.9.586; v0.9.589: the radio, on her hip, not her head) muffled while the radio is under the water's surface:
    // Lara in a water volume and the radio's height below the surface's (2 cm either way before it changes, against
    // the ripple); without the radio's place, her head under water as the swim code sees it.
    {
        const bool was = g_laraUnderwater.load(std::memory_order_relaxed);
        bool under = false;
        float surface = 0.0f;
        const bool place = radioPlaced;
        if (place && Features::PlayerWaterSurfaceZ(surface))
            under = was ? target[2] < surface + 2.0f : target[2] < surface - 2.0f;
        else if (!place)
            under = SwimBreath::LaraUnderwater();
        if (under != was) {
            g_laraUnderwater.store(under, std::memory_order_relaxed);
            static int s_logs = 0;
            if (s_logs++ < 8 && g_underwaterMuffle.load(std::memory_order_relaxed))
                Log::Detailf("LaraFM: %s", under ? (place ? "her radio under water: the music muffled"
                                                          : "Lara under water: the music muffled")
                                                 : "out of the water: the music clear again");
        }
    }

    float gain = 1.0f, pan = 0.0f;
    Features::SnapshotDrawCameraNow();
    Features::DrawCameraState camera{};
    if (g_sourceValid && Features::GetDrawCameraState(camera) && camera.valid) {
        float qx = camera.quaternion[0], qy = camera.quaternion[1], qz = camera.quaternion[2], qw = camera.quaternion[3];
        const float len = std::sqrt(qx * qx + qy * qy + qz * qz + qw * qw);
        if (len > 1.0e-5f) {
            qx /= len; qy /= len; qz /= len; qw /= len;
            const float right[3] = {1.0f - 2.0f * (qy * qy + qz * qz), 2.0f * (qx * qy + qz * qw), 2.0f * (qx * qz - qy * qw)};
            const float forward[3] = {2.0f * (qx * qy - qz * qw), 1.0f - 2.0f * (qx * qx + qz * qz), 2.0f * (qy * qz + qx * qw)};
            float dir[3] = {g_sourcePos[0] - camera.position[0], g_sourcePos[1] - camera.position[1],
                            g_sourcePos[2] - camera.position[2]};
            const float d = std::sqrt(dir[0] * dir[0] + dir[1] * dir[1] + dir[2] * dir[2]);
            const float metres = d / 100.0f;
            // full up to 6 m (the camera behind her), then falling as 1/distance, gone by 60 m (a camera far off)
            gain = 6.0f / std::max(metres, 6.0f);
            if (metres > 40.0f) gain *= std::clamp((60.0f - metres) / 20.0f, 0.0f, 1.0f);
            if (d > 1.0f) {
                for (float& v : dir) v /= d;
                const float side = dir[0] * right[0] + dir[1] * right[1] + dir[2] * right[2];
                const float front = dir[0] * forward[0] + dir[1] * forward[1] + dir[2] * forward[2];
                // (close by it pans little: within a metre or two it is all around)
                const float spread = std::clamp((metres - 1.0f) / 3.0f, 0.0f, 1.0f);
                pan = side * spread * 0.4f;
                if (front < 0.0f) gain *= 1.0f + 0.2f * front;   // (a little quieter behind)
            }
        }
    }
    g_mixGain.store(gain, std::memory_order_relaxed);
    g_mixPan.store(pan, std::memory_order_relaxed);
    std::lock_guard<std::mutex> lock(g_sourceMutex);
    g_sourceText = text;
}

void HandleKey(unsigned int msg, unsigned long long wParam, long long lParam) {
    if (msg != WM_KEYDOWN || (static_cast<unsigned long long>(lParam) & (1ull << 30))) return;   // (not the repeats)
    if (wParam != VK_UP && wParam != VK_DOWN && wParam != VK_LEFT && wParam != VK_RIGHT) return;
    if (!g_available.load() || !g_arrowKeys.load() || Overlay::OwnsInput() || Features::MenuPaused() ||
        Features::LoadingProfilerSessionActive() || g_hasRadio.load() == 0)
        return;
    switch (wParam) {
    case VK_UP: Push(Command::Up); break;
    case VK_DOWN: Push(Command::TogglePause); break;
    case VK_LEFT: Push(Command::Prev); break;
    case VK_RIGHT: Push(Command::Next); break;
    default: break;
    }
}

void DrawTab() {
    if (!g_available.load()) {
        ImGui::TextWrapped("LaraFM is unavailable: the game's sound engine did not match. See ROTTRFramework.log.");
        return;
    }
    Status status;
    {
        std::lock_guard<std::mutex> lock(g_statusMutex);
        status = g_status;
    }
    std::string source;
    {
        std::lock_guard<std::mutex> lock(g_sourceMutex);
        source = g_sourceText;
    }
    Config config = CurrentConfig();
    bool changed = false;

    ImGui::TextUnformatted("Your own music on a radio in the game.");
    ImGui::Separator();
    auto clock = [](unsigned ms) {
        char buf[32];
        const unsigned s = ms / 1000;
        std::snprintf(buf, sizeof(buf), "%u:%02u", s / 60, s % 60);
        return std::string(buf);
    };
    switch (status.state) {
    case State::Stopped: ImGui::TextDisabled("Stopped"); break;
    case State::Loading: ImGui::Text("Loading: %s", status.track.c_str()); break;
    case State::Playing:
    case State::Paused:
        ImGui::Text("%s: %s", status.state == State::Paused ? "Paused" : "Playing", status.track.c_str());
        if (status.lengthMs) {
            const float f = std::clamp(static_cast<float>(status.positionMs) / static_cast<float>(status.lengthMs), 0.0f, 1.0f);
            const std::string label = clock(status.positionMs) + " / " + clock(status.lengthMs);
            ImGui::ProgressBar(f, ImVec2(-1.0f, 0.0f), label.c_str());
        } else {
            ImGui::Text("%s", clock(status.positionMs).c_str());
        }
        ImGui::TextDisabled("From: %s", source.c_str());
        if (status.hold && status.hold != kNoRadio) ImGui::TextDisabled("Silent for now: %s", status.hold);
        break;
    }
    if (!status.error.empty()) ImGui::TextColored(ImVec4(1.0f, 0.55f, 0.35f, 1.0f), "%s", status.error.c_str());
    // (v0.9.495) no radio in her inventory: nothing to listen on, and the way to give her one
    if (g_hasRadio.load() == 0) {
        ImGui::Spacing();
        ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.35f, 1.0f), "%s", kNoRadio);
        if (ImGui::Button("Give Lara her radio")) g_giveRadio.store(true);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Adds her walkie-talkie (%s) to her inventory, the way the game gives a missing item.",
                              kLaraRadioObject);
    }
    {
        std::string give;
        {
            std::lock_guard<std::mutex> lock(g_sourceMutex);
            give = g_giveText;
        }
        if (!give.empty()) ImGui::TextDisabled("%s", give.c_str());
    }

    static int s_selected = -1;
    ImGui::Spacing();
    if (ImGui::Button("Previous")) Push(Command::Prev);
    ImGui::SameLine();
    if (ImGui::Button("Play")) Push(Command::Play, s_selected);
    ImGui::SameLine();
    if (ImGui::Button("Pause")) Push(Command::Pause);
    ImGui::SameLine();
    if (ImGui::Button("Stop")) Push(Command::Stop);
    ImGui::SameLine();
    if (ImGui::Button("Next")) Push(Command::Next);
    ImGui::SameLine();
    if (ImGui::Checkbox("Shuffle", &config.shuffle)) {
        g_shuffle.store(config.shuffle);
        changed = true;
    }

    const float width = ImGui::GetFontSize() * 14.0f;
    float volume = config.volume * 100.0f;
    ImGui::SetNextItemWidth(width);
    if (ImGui::SliderFloat("Volume", &volume, 0.0f, 100.0f, "%.0f%%", ImGuiSliderFlags_AlwaysClamp)) {
        config.volume = volume / 100.0f;
        g_volume.store(config.volume);
        changed = true;
    }
    float effect = config.radioEffect * 100.0f;
    ImGui::SetNextItemWidth(width);
    if (ImGui::SliderFloat("Radio effect", &effect, 0.0f, 100.0f, "%.0f%%", ImGuiSliderFlags_AlwaysClamp)) {
        config.radioEffect = effect / 100.0f;
        g_radioEffect.store(config.radioEffect);
        changed = true;
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("How much it sounds like an old radio: 0%% is the music as it is, 100%% a small mono\n"
                          "speaker (no deep bass or high treble, a little distortion and hiss). At once.");
    if (ImGui::Checkbox("Muffled under water", &config.underwaterMuffle)) {   // (v0.9.586)
        g_underwaterMuffle.store(config.underwaterMuffle);
        changed = true;
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("While her radio (on her hip) is under water the music sounds as heard from under water:\n"
                          "its low sounds only. Wading deep enough muffles it too. It comes back clear out of the water.");
    if (!config.underwaterMuffle) ImGui::BeginDisabled();
    float underwater = config.underwaterVolume * 100.0f;   // (v0.9.589)
    ImGui::SetNextItemWidth(width);
    if (ImGui::SliderFloat("Under water volume", &underwater, 25.0f, 150.0f, "%.0f%%", ImGuiSliderFlags_AlwaysClamp)) {
        config.underwaterVolume = underwater / 100.0f;
        g_underwaterVolume.store(config.underwaterVolume);
        changed = true;
    }
    if (!config.underwaterMuffle) ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("How loud the muffled music is: 100%% keeps its low sounds as loud as out of the water.\n"
                          "At once.");
    if (ImGui::Checkbox("Play during cutscenes", &config.playInCutscenes)) {   // (v0.9.590)
        g_playInCutscenes.store(config.playInCutscenes);
        changed = true;
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Off: the music pauses while a cutscene plays and goes on where it was after it.");
    ImGui::Spacing();
    if (ImGui::Checkbox("Arrow keys", &config.arrowKeys)) {
        g_arrowKeys.store(config.arrowKeys);
        changed = true;
    }
    ImGui::SameLine();
    ImGui::TextDisabled("Up play, Down pause, Left previous, Right next (while you play)");

    ImGui::Spacing();
    ImGui::Separator();
    const std::wstring folder = FolderPath();
    if (ImGui::Button("Open folder")) {
        std::error_code ec;
        std::filesystem::create_directories(folder, ec);
        ShellExecuteW(nullptr, L"open", folder.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    }
    ImGui::SameLine();
    if (ImGui::Button("Refresh")) ScanTracks();
    ImGui::SameLine();
    std::vector<Track> tracks;
    {
        std::lock_guard<std::mutex> lock(g_tracksMutex);
        tracks = g_tracks;
    }
    ImGui::TextDisabled("%d track(s) in ROTTRFramework\\LaraFM", static_cast<int>(tracks.size()));
    if (tracks.empty()) {
        ImGui::TextWrapped("Put mp3, m4a, mp4, aac, wav, flac, wma or ogg files in ROTTRFramework\\LaraFM (in the game's "
                           "folder; subfolders too), then Refresh.");
    } else if (ImGui::BeginListBox("##larafm_tracks", ImVec2(-1.0f, ImGui::GetTextLineHeightWithSpacing() * 12.0f))) {
        for (int i = 0; i < static_cast<int>(tracks.size()); ++i) {
            const bool current = i == status.index && status.state != State::Stopped;
            std::string label = (current ? "> " : "  ") + tracks[static_cast<std::size_t>(i)].name + "##t" + std::to_string(i);
            if (ImGui::Selectable(label.c_str(), s_selected == i, ImGuiSelectableFlags_AllowDoubleClick)) {
                s_selected = i;
                if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) Push(Command::PlayIndex, i);
            }
        }
        ImGui::EndListBox();
    }
    ImGui::TextDisabled("Click a track to choose it, double-click to play it.");

    // (saved once nothing is being dragged or typed in any more)
    static bool s_dirty = false;
    if (changed) {
        std::lock_guard<std::mutex> lock(g_configMutex);
        g_config = config;
        s_dirty = true;
    }
    if (s_dirty && !ImGui::IsAnyItemActive()) {
        SaveConfig();
        s_dirty = false;
    }
}
}  // namespace LaraFM
