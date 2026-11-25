// The Model Viewer's sound (see viewer_audio.h). Addresses: the retail Steam ROTTR.exe; every function called and every
// place read is checked against the code that uses it (Check).
#include "viewer_audio.h"

#include "dtp_edit.h"
#include "game_build.h"
#include "log.h"

#include <Windows.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <iterator>
#include <map>
#include <string>
#include <vector>

namespace ViewerAudio {
namespace {
// cdc::SOUND_Play(Handle<SoundOwner>* out, dtp::SoundPlex*): SoundPlexCollection::StartPaused (sm_Collection,
// 0x2400D90), then the owner's pause bit (+0x2F4 bit 0) cleared; cdc::SOUND_Play(out, unsigned id): the same with the
// DTP's data.
constexpr std::uintptr_t kRvaPlayPlex = 0x003B8570ull, kRvaPlayDtp = 0x003B8540ull;
constexpr std::uintptr_t kRvaSoundOwners = 0x02400D90ull, kRvaGlobalSoundInfo = 0x0233C488ull, kRvaSoundGroups = 0x00FF7380ull;
// cdc::Handle<SoundOwner>'s HandleData (16 bytes: the owner, its count): back to the pool's free list (+0) under its
// SRW lock when the count is 0, as the game's handles do (SOUND_Play's own, 0x3B8592..0x3B85D3)
constexpr std::uintptr_t kRvaHandleLock = 0x0246B150ull, kRvaHandleFree = 0x0246B140ull;
constexpr std::uintptr_t kRvaAcquireSlot = 0x00D019E8ull, kRvaReleaseSlot = 0x00D019F0ull;
// cdc::SoundOwner: its sound group (+0x94, the Assignment's), m_controls (+0x2D8, cdc::SoundTypes::Controls, its first
// float the volume: what the action graphs' sound nodes set on a playing sound, retail 0x63B0E0), m_dtpRootOwnerID
// (+0x34C), its end (+0x3F4: 0xFF none yet, 6 at once: SOUND_StopAllSound's)
constexpr std::size_t kOwnerGroup = 0x94, kOwnerVolume = 0x2D8, kOwnerRoot = 0x34C, kOwnerEnd = 0x3F4, kNodeOwner = 0x10;
constexpr std::uint8_t kOwnerEndNone = 0xFF, kOwnerEndNow = 6;
// cdc::gSoundGroups (+0 the groups, +8 their count): dtp::SoundGroup, 0x1C0 each, name +0x148, parent +0x1B0.
// GlobalSoundInfo +0x68 soundGroupAssignments (musicSlider +0, music +0xC)
constexpr std::size_t kGroupSize = 0x1C0, kGroupName = 0x148, kGroupParent = 0x1B0;
constexpr std::size_t kInfoAssignments = 0x68, kAssignMusicSlider = 0x00, kAssignMusic = 0x0C;
constexpr int kMaxOwners = 4096;

struct Sig {
    std::uintptr_t rva;
    std::uint8_t bytes[24];
    std::size_t size;
};
const Sig kSigs[] = {
    {kRvaPlayPlex, {0x48, 0x89, 0x5C, 0x24, 0x08, 0x57, 0x48, 0x83, 0xEC, 0x20, 0x4C, 0x8B, 0xC2, 0x48, 0x8B, 0xD9, 0x48, 0x8B,
                    0xD1, 0x0F, 0x57, 0xDB, 0x48, 0x8D}, 24},
    {kRvaPlayDtp, {0x40, 0x53, 0x48, 0x83, 0xEC, 0x20, 0x44, 0x8B, 0xC2, 0x48, 0x8B, 0xD9, 0x48, 0x8B, 0xD1, 0x48, 0x8D, 0x0D}, 18},
    // SOUND_PauseGroups' read of a group (0x1C0 each, from gSoundGroups)
    {0x003B7F48ull, {0x49, 0x69, 0xDA, 0xC0, 0x01, 0x00, 0x00, 0x41, 0x8B, 0xC2, 0x48, 0x03, 0x1D}, 13},
    // SoundPlexCollection::StartPaused: the owner's group against GlobalSoundInfo +0x68 +0xC; a group's parent; the
    // owner's root DTP
    {0x003BB106ull, {0x8B, 0x83, 0x94, 0x00, 0x00, 0x00, 0x48, 0x8B, 0x51, 0x68, 0x44, 0x8B, 0x42, 0x0C}, 14},
    {0x003BB12Dull, {0x48, 0x69, 0xC8, 0xC0, 0x01, 0x00, 0x00, 0x8B, 0x84, 0x11, 0xB0, 0x01, 0x00, 0x00}, 14},
    {0x003BB199ull, {0x8B, 0x93, 0x4C, 0x03, 0x00, 0x00, 0x49, 0x8D, 0x4F, 0x08}, 10},
    // SOUND_StopAllSound's walk of the owners (+0x3F4 = 5 with a fade, 6 at once)
    {0x003B8B4Dull, {0xC6, 0x81, 0xF4, 0x03, 0x00, 0x00, 0x05}, 7},
    {0x003B8B79ull, {0xC6, 0x81, 0xF4, 0x03, 0x00, 0x00, 0x06}, 7},
    // the action graphs' sound volume node: the owner's controls' volume set (movss [rcx+0x2D8], xmm1; ret)
    {0x0063B0E0ull, {0xF3, 0x0F, 0x11, 0x89, 0xD8, 0x02, 0x00, 0x00, 0xC3}, 9},
    // SoundPlexAssignment's ctor: the options' group to the owner
    {0x00395ECAull, {0x49, 0x8B, 0x47, 0x30, 0x8B, 0x48, 0x28, 0x89, 0x8D, 0x94, 0x00, 0x00, 0x00}, 13},
    // SOUND_Play's handle let go: the owner gone, the count down, the node to the free list under the lock
    {0x003B8592ull, {0x48, 0x8B, 0x3B, 0x48, 0x85, 0xFF, 0x74, 0x55, 0x48, 0x8B, 0x07, 0x48, 0x85, 0xC0, 0x75, 0x46, 0x83, 0x6F,
                     0x08, 0x01, 0x75, 0x2B}, 22},
    {0x003B85C3ull, {0x48, 0x89, 0x07}, 3},
    // PlexFactory::CreatePlexHierarchy's switch on the sound's type (the table at 0x39F940), its Stream's (4) and its
    // Assignment's (6) constructors called
    {0x0039F280ull, {0x0F, 0xB7, 0x07, 0x83, 0xF8, 0x10, 0x0F, 0x87, 0xA9, 0x06, 0x00, 0x00, 0x41, 0x8B, 0x8C, 0x84, 0x40, 0xF9,
                     0x39, 0x00, 0x49, 0x03, 0xCC, 0xFF}, 24},
    {0x0039F950ull, {0x75, 0xF6, 0x39, 0x00}, 4},
    {0x0039F958ull, {0x31, 0xF7, 0x39, 0x00}, 4},
    {0x0039F6C9ull, {0xE8, 0x22, 0x77, 0xFF, 0xFF}, 5},
    {0x0039F785ull, {0xE8, 0x86, 0x65, 0xFF, 0xFF}, 5},
    // SoundPlexStream's ctor (its data at +0x60, the group read) and FillOutCriticalInfo (the name +0, the priority
    // +0xD, the channels +8)
    {0x00396E80ull, {0x48, 0x8B, 0x47, 0x10, 0x48, 0x89, 0x5F, 0x60, 0x89, 0x4F, 0x68, 0xC6, 0x47, 0x3B, 0x04, 0x8B, 0x88, 0x84,
                     0x00, 0x00, 0x00}, 21},
    {0x003A2B7Bull, {0x48, 0x8B, 0x43, 0x20, 0x48, 0x8B, 0x08, 0x49, 0x89, 0x0F, 0x48, 0x8B, 0x43, 0x20, 0x0F, 0xBE, 0x48, 0x0D,
                     0x41, 0x89, 0x0E, 0x48, 0x8B, 0x43}, 24},
};
// RIP-relative operands: the instruction at rva (prefix bytes, its length, its disp32's place) names target
struct Rip {
    std::uintptr_t rva;
    std::uint8_t prefix[3];
    std::size_t prefixSize, length, disp;
    std::uintptr_t target;
};
const Rip kRips[] = {
    {0x003B8586ull, {0x48, 0x8D, 0x0D}, 3, 7, 3, kRvaSoundOwners},       // SOUND_Play (sound): lea rcx, sm_Collection
    {0x003B854Full, {0x48, 0x8D, 0x0D}, 3, 7, 3, kRvaSoundOwners},       // SOUND_Play (DTP)
    {0x003B7F52ull, {0x48, 0x03, 0x1D}, 3, 7, 3, kRvaSoundGroups},       // SOUND_PauseGroups: add rbx, gSoundGroups
    {0x003BB0FFull, {0x48, 0x8B, 0x0D}, 3, 7, 3, kRvaGlobalSoundInfo},   // StartPaused: mov rcx, GlobalSoundInfo
    {0x003B8B29ull, {0x48, 0x8B, 0x0D}, 3, 7, 3, kRvaSoundOwners},       // SOUND_StopAllSound: the owners' list
    {0x003B85A8ull, {0x48, 0x8D, 0x0D}, 3, 7, 3, kRvaHandleLock},        // lea rcx, the handles' lock
    {0x003B85AFull, {0xFF, 0x15, 0x00}, 2, 6, 2, kRvaAcquireSlot},       // call [AcquireSRWLockExclusive]
    {0x003B85B5ull, {0x48, 0x8B, 0x05}, 3, 7, 3, kRvaHandleFree},        // mov rax, the free list
    {0x003B85C6ull, {0x48, 0x89, 0x3D}, 3, 7, 3, kRvaHandleFree},        // mov the free list, rdi
    {0x003B85CDull, {0xFF, 0x15, 0x00}, 2, 6, 2, kRvaReleaseSlot},       // call [ReleaseSRWLockExclusive]
};

// The sound we play: as the game's relic lines are (globaluiinfo.drm), an Assignment over a Stream.
#pragma pack(push, 8)
struct Plex {   // dtp::SoundPlex
    std::uint16_t type;
    std::uint8_t clickPlay;
    std::uint8_t pad0[5];
    const void* data;
    std::uint32_t ownerId;
    std::uint32_t pad1;
};
struct Stream {   // dtp::SoundPlex::Stream
    const char* name;
    std::int32_t channels;
    std::int8_t subtitleMode;   // 0 default, 1 always, 2 never
    std::int8_t priority;
    std::uint8_t cull;
    std::uint8_t pad;
};
struct Options {   // dtp::SoundPlex::Assignment::AssignmentOptions
    float initialDelay, initialDelayVariation, volumeVariation, pitchVariation, reverb;
    std::uint16_t lowPass, highPass;
    float rolloffScale, rolloffScaleReverb, timeout, timeoutVariation;
    std::uint32_t soundGroup;
    float pan, levelOfWide3D, levelOfLfe, maxDistance, minDistance, cullRadiusOffset;
    std::uint8_t distVolCurve, playbackType, maxInstances, flags;
};
struct Assignment {   // dtp::SoundPlex::Assignment
    const Plex* sound;
    void* maxHead;   // m_maxInstanceList (the game's)
    void* maxTail;
    std::uint16_t maxCount;
    std::uint8_t pad0[6];
    std::uint8_t curInstances;
    std::uint8_t pad1[7];
    const void* panning;   // dtp::PanningOptions
    const Options* options;
    float volume, pitch;
    std::uint32_t lastDumpCheck;
    std::uint32_t pad2;
};
#pragma pack(pop)
static_assert(sizeof(Plex) == 0x18 && sizeof(Stream) == 0x10 && sizeof(Options) == 0x48 && sizeof(Assignment) == 0x48,
              "the game's sound structures");
static_assert(offsetof(Options, lowPass) == 0x14 && offsetof(Options, soundGroup) == 0x28 && offsetof(Options, pan) == 0x2C &&
                  offsetof(Options, distVolCurve) == 0x44,
              "AssignmentOptions");
static_assert(offsetof(Assignment, curInstances) == 0x20 && offsetof(Assignment, panning) == 0x28 &&
                  offsetof(Assignment, options) == 0x30 && offsetof(Assignment, volume) == 0x38,
              "Assignment");
static_assert(offsetof(Stream, channels) == 8 && offsetof(Stream, subtitleMode) == 12 && offsetof(Stream, priority) == 13,
              "Stream");
struct alignas(16) Music {
    Plex root;
    Assignment assignment;
    Options options;
    std::uint64_t panning[2];
    Plex streamPlex;
    Stream stream;
    char name[256];
};
// (a new one when what it is made of changes, the one before kept: the game may still be fading it out, its Assignment's
// instance list the game's)
Music* g_music = nullptr;
bool g_musicBuilt = false;
std::string g_musicBuiltFor;

int g_codeState = 0;   // 0 not checked, 1 as expected, -1 not
bool g_started = false;   // the viewer open
void* g_musicHandle = nullptr;
void* g_voiceHandle = nullptr;
std::string g_track;
std::uint32_t g_ownerDtp = 0;
unsigned g_musicGroup = 0;
double g_musicStartedAt = 0.0;
int g_quickEnds = 0, g_restarts = 0;
bool g_musicGaveUp = false;
std::vector<unsigned> g_voiceGroups;
int g_voiceLogs = 0;

// The volumes (v0.9.433): ours faded in when the viewer opens, lowered under a relic's line, faded out when it closes
// (then ended); the level's sounds (the storm's, the menu's music) faded out the while and back in after. Written into
// each owner's controls every frame (game thread, CutsceneViewer's tick: after closing too, for the fades).
constexpr double kMusicIn = 2.0, kMusicOut = 1.2;   // s, from silence to full and back
constexpr double kDuckLevel = 0.35, kDuckDown = 0.35, kDuckUp = 1.0;   // under a line: the level, the s to it and back
constexpr double kQuietOut = 1.0, kQuietIn = 1.5;    // the level's sounds: out when it opens, back when it closes
double g_musicLevel = 0.0;      // what ours plays at (of its own volume, 1)
bool g_musicFadingIn = false;   // from opening until it is at full
bool g_musicStopping = false;   // fading out after closing
float g_musicWritten = -1.0f;   // the volume written last (an owner's controls changed by another: counted)
int g_musicOverwritten = 0, g_ducks = 0;
double g_lastTick = 0.0;
struct Quiet {   // one of the level's sounds faded out
    std::uint8_t* owner;
    std::uint32_t root;
    unsigned group;
    float volume;    // its own, when first seen
    float written;
};
std::vector<Quiet> g_quiet;
double g_quietLevel = 1.0;          // the fraction of their own volume they play at
int g_quietOverwritten = 0, g_quietSeen = 0;
std::vector<std::int8_t> g_quietGroups;   // each sound group: 1 the level's (faded), 0 not, -1 not yet known
unsigned g_groupCount = 0;
const std::uint8_t* g_groupList = nullptr;
unsigned g_music1 = 0, g_music2 = 0;     // the music group and the music volume's

double Now() {
    static LARGE_INTEGER frequency{};
    if (!frequency.QuadPart) QueryPerformanceFrequency(&frequency);
    LARGE_INTEGER t{};
    QueryPerformanceCounter(&t);
    return double(t.QuadPart) / double(frequency.QuadPart);
}

bool CodeMatches() {
    if (g_codeState) return g_codeState > 0;
    const std::uintptr_t base = GameBuild::Base();
    bool ok = base != 0;
    for (const Sig& s : kSigs) {
        if (!ok) break;
        if (!GameBuild::VerifyBytes(s.rva, s.bytes, s.size)) {
            Log::Writef("ModelViewer: the game's sound code at 0x%llX is not the code expected; no music, no relic lines",
                        static_cast<unsigned long long>(s.rva));
            ok = false;
        }
    }
    for (const Rip& r : kRips) {
        if (!ok) break;
        const auto* code = reinterpret_cast<const std::uint8_t*>(base + r.rva);
        const std::uintptr_t target =
            r.rva + r.length + static_cast<std::intptr_t>(*reinterpret_cast<const std::int32_t*>(code + r.disp));
        if (std::memcmp(code, r.prefix, r.prefixSize) != 0 || target != r.target) {
            Log::Writef("ModelViewer: the game's sound code at 0x%llX does not name 0x%llX; no music, no relic lines",
                        static_cast<unsigned long long>(r.rva), static_cast<unsigned long long>(r.target));
            ok = false;
        }
    }
    if (ok) {   // (the handles' lock: the SRW lock functions the game imports)
        HMODULE kernel = GetModuleHandleW(L"kernel32.dll");
        const void* acquire = kernel ? reinterpret_cast<const void*>(GetProcAddress(kernel, "AcquireSRWLockExclusive")) : nullptr;
        const void* release = kernel ? reinterpret_cast<const void*>(GetProcAddress(kernel, "ReleaseSRWLockExclusive")) : nullptr;
        const void* slotA = *reinterpret_cast<void* const*>(base + kRvaAcquireSlot);
        const void* slotR = *reinterpret_cast<void* const*>(base + kRvaReleaseSlot);
        if (!acquire || !release || slotA != acquire || slotR != release) {
            Log::Write("ModelViewer: the sound handles' lock is not the SRW lock expected; no music, no relic lines");
            ok = false;
        }
    }
    g_codeState = ok ? 1 : -1;
    return ok;
}

using PlayPlexFn = void**(__fastcall*)(void**, const void*);
using PlayDtpFn = void**(__fastcall*)(void**, std::uint32_t);

// (SEH only, these)
void* PlayOurSound(const void* plex) noexcept {
    void* handle = nullptr;
    __try {
        reinterpret_cast<PlayPlexFn>(GameBuild::Base() + kRvaPlayPlex)(&handle, plex);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
    return handle;
}
void* PlayDtp(std::uint32_t id) noexcept {
    void* handle = nullptr;
    __try {
        reinterpret_cast<PlayDtpFn>(GameBuild::Base() + kRvaPlayDtp)(&handle, id);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
    return handle;
}
std::uint8_t* OwnerOf(void* handle) noexcept {
    if (!handle) return nullptr;
    __try {
        return *static_cast<std::uint8_t**>(handle);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}
void EndOwner(std::uint8_t* owner) noexcept {
    if (!owner) return;
    __try {
        if (owner[kOwnerEnd] == kOwnerEndNone) owner[kOwnerEnd] = kOwnerEndNow;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}
// An owner's volume (its controls'): read, written. -1: not readable.
float VolumeOf(const std::uint8_t* owner) noexcept {
    if (!owner) return -1.0f;
    __try {
        return *reinterpret_cast<const float*>(owner + kOwnerVolume);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1.0f;
    }
}
void SetVolumeOf(std::uint8_t* owner, float volume) noexcept {
    if (!owner) return;
    __try {
        *reinterpret_cast<float*>(owner + kOwnerVolume) = volume;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}
// The handle let go (its count down; at 0 the node back to the pool).
bool DropCount(std::uint8_t* node) noexcept {
    __try {
        return --*reinterpret_cast<std::int32_t*>(node + 8) == 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
void ReleaseHandle(void*& handle) {
    auto* node = static_cast<std::uint8_t*>(handle);
    handle = nullptr;
    if (!node || !DropCount(node)) return;
    const std::uintptr_t base = GameBuild::Base();
    auto* lock = reinterpret_cast<PSRWLOCK>(base + kRvaHandleLock);
    auto** head = reinterpret_cast<void**>(base + kRvaHandleFree);
    AcquireSRWLockExclusive(lock);
    *reinterpret_cast<void**>(node) = *head;
    *head = node;
    ReleaseSRWLockExclusive(lock);
}

// The sound groups (SEH only): the list, the music's two.
bool ReadGroups() noexcept {
    __try {
        const std::uintptr_t base = GameBuild::Base();
        const auto* info = *reinterpret_cast<const std::uint8_t* const*>(base + kRvaGlobalSoundInfo);
        g_groupList = *reinterpret_cast<const std::uint8_t* const*>(base + kRvaSoundGroups);
        g_groupCount = *reinterpret_cast<const unsigned*>(base + kRvaSoundGroups + 8);
        if (!info || !g_groupList || !g_groupCount || g_groupCount > 4096) return false;
        const auto* assignments = *reinterpret_cast<const std::uint8_t* const*>(info + kInfoAssignments);
        if (!assignments) return false;
        g_music2 = *reinterpret_cast<const unsigned*>(assignments + kAssignMusicSlider);
        g_music1 = *reinterpret_cast<const unsigned*>(assignments + kAssignMusic);
        return g_music1 < g_groupCount && g_music2 < g_groupCount;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        g_groupCount = 0;
        return false;
    }
}
unsigned ParentOf(unsigned group) noexcept {
    __try {
        return group < g_groupCount ? *reinterpret_cast<const unsigned*>(g_groupList + std::size_t(group) * kGroupSize + kGroupParent) : 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}
const char* NameOf(unsigned group, char* out, std::size_t size) noexcept {
    out[0] = '\0';
    __try {
        const char* name = group < g_groupCount
                               ? *reinterpret_cast<const char* const*>(g_groupList + std::size_t(group) * kGroupSize + kGroupName)
                               : nullptr;
        std::size_t k = 0;
        for (; name && k + 1 < size && name[k]; ++k) out[k] = name[k];
        out[k] = '\0';
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        out[0] = '\0';
    }
    if (!out[0]) std::snprintf(out, size, "%u", group);
    return out;
}
// The group, or one of its parents (up to the master group, 0), is one of these.
bool Under(unsigned group, std::initializer_list<unsigned> list) {
    for (int steps = 0; steps < 32 && group < g_groupCount; ++steps) {
        if (std::find(list.begin(), list.end(), group) != list.end()) return true;
        if (group == 0) return false;
        const unsigned parent = ParentOf(group);
        if (parent == group) return false;
        group = parent;
    }
    return false;
}
// One of the level's sounds' groups, to fade out: the environment's ("sfx\env..."), the menu's music (the music's
// groups; ours is told apart by its owner).
bool QuietGroup(unsigned group) {
    if (group >= g_groupCount) return false;
    if (g_quietGroups.size() != g_groupCount) g_quietGroups.assign(g_groupCount, -1);
    std::int8_t& known = g_quietGroups[group];
    if (known < 0) {
        bool quiet = Under(group, {g_music1, g_music2});
        for (unsigned at = group, steps = 0; !quiet && steps < 32 && at < g_groupCount; ++steps) {
            char name[96];
            NameOf(at, name, sizeof(name));
            quiet = _strnicmp(name, "sfx\\env", 7) == 0;
            if (at == 0) break;
            const unsigned parent = ParentOf(at);
            if (parent == at) break;
            at = parent;
        }
        known = quiet ? 1 : 0;
    }
    return known > 0;
}
// The sounds playing: their owners, groups and root DTPs. SEH only.
int PlayingOwners(std::uint8_t** owners, unsigned* groups, std::uint32_t* roots, int max) noexcept {
    int n = 0;
    __try {
        auto* head = *reinterpret_cast<std::uint8_t**>(GameBuild::Base() + kRvaSoundOwners);
        if (!head) return 0;
        int steps = 0;
        for (auto* node = *reinterpret_cast<std::uint8_t**>(head); node && node != head && steps < kMaxOwners && n < max;
             node = *reinterpret_cast<std::uint8_t**>(node), ++steps) {
            auto* owner = *reinterpret_cast<std::uint8_t* const*>(node + kNodeOwner);
            if (!owner) continue;
            owners[n] = owner;
            groups[n] = *reinterpret_cast<const unsigned*>(owner + kOwnerGroup);
            roots[n] = *reinterpret_cast<const std::uint32_t*>(owner + kOwnerRoot);
            ++n;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    return n;
}
// A sound DTP's group: an Assignment's (its root's type 6). SEH only.
bool DtpGroup(std::uint32_t id, unsigned& group) noexcept {
    const auto* plex = static_cast<const std::uint8_t*>(DtpEdit::DataPointer(id));
    if (!plex) return false;
    __try {
        if (*reinterpret_cast<const std::uint16_t*>(plex) != 6) return false;
        const auto* assignment = *reinterpret_cast<const std::uint8_t* const*>(plex + 8);
        const auto* options = assignment ? *reinterpret_cast<const std::uint8_t* const*>(assignment + offsetof(Assignment, options)) : nullptr;
        if (!options) return false;
        group = *reinterpret_cast<const unsigned*>(options + offsetof(Options, soundGroup));
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// Ours, as globaluiinfo.drm's relic lines are (DTP 193891: the options' values, the Assignment's volume 1 and pitch 0,
// eight bytes of zero panning, the Stream's priority 90 and its cull; here no cull, no subtitles, the music's group).
void BuildMusic(const std::string& track, unsigned group, std::uint32_t owner) {
    g_music = new Music{};
    Music& m = *g_music;
    strncpy_s(m.name, track.c_str(), _TRUNCATE);
    m.stream.name = m.name;
    m.stream.channels = 2;
    m.stream.subtitleMode = 2;
    m.stream.priority = 95;
    m.stream.cull = 0;
    m.streamPlex.type = 4;   // Stream
    m.streamPlex.data = &m.stream;
    m.streamPlex.ownerId = owner;
    m.options.lowPass = 22000;
    m.options.highPass = 0;
    m.options.rolloffScale = m.options.rolloffScaleReverb = 1.0f;
    m.options.soundGroup = group;
    m.options.pan = -2.0f;
    m.options.levelOfWide3D = m.options.levelOfLfe = m.options.maxDistance = m.options.minDistance = m.options.cullRadiusOffset = -1.0f;
    m.assignment.sound = &m.streamPlex;
    m.assignment.panning = m.panning;
    m.assignment.options = &m.options;
    m.assignment.volume = 1.0f;
    m.assignment.pitch = 0.0f;
    m.root.type = 6;   // Assignment
    m.root.data = &m.assignment;
    m.root.ownerId = owner;
    g_musicBuilt = true;
    g_musicBuiltFor = track + "|" + std::to_string(group) + "|" + std::to_string(owner);
}

bool StartMusic(const char* why) {
    g_musicHandle = PlayOurSound(&g_music->root);
    g_musicStartedAt = Now();
    std::uint8_t* owner = OwnerOf(g_musicHandle);
    if (!owner) {
        ReleaseHandle(g_musicHandle);
        Log::Writef("ModelViewer: the music (%s) did not start (%s): the game gave no sound owner", g_track.c_str(), why);
        return false;
    }
    // (at once, at the level it is at: the game's sound update is still to come, nothing of it heard yet)
    g_musicWritten = float(g_musicLevel);
    SetVolumeOf(owner, g_musicWritten);
    return true;
}

// The music let go at once (closing's fade done, or the viewer opened again before it was).
void EndMusic() {
    EndOwner(OwnerOf(g_musicHandle));
    ReleaseHandle(g_musicHandle);
    g_musicStopping = false;
}

// The level's sounds: the ones playing now and any that start while the viewer is open faded out (each to its own
// volume x the level), and back after it closes; let go once back.
void QuietSounds(double level, bool takeNew) {
    static std::uint8_t* owners[512];
    static unsigned groups[512];
    static std::uint32_t roots[512];
    const int playing = g_groupCount ? PlayingOwners(owners, groups, roots, 512) : 0;
    const std::uint8_t* ours = OwnerOf(g_musicHandle);
    // the ones gone (or another sound in their place) left
    g_quiet.erase(std::remove_if(g_quiet.begin(), g_quiet.end(),
                                 [&](const Quiet& q) {
                                     for (int i = 0; i < playing; ++i)
                                         if (owners[i] == q.owner) return roots[i] != q.root || groups[i] != q.group;
                                     return true;
                                 }),
                  g_quiet.end());
    if (takeNew)
        for (int i = 0; i < playing; ++i) {
            if (owners[i] == ours || (roots[i] == g_ownerDtp && groups[i] == g_musicGroup) || !QuietGroup(groups[i])) continue;
            if (std::any_of(g_quiet.begin(), g_quiet.end(), [&](const Quiet& q) { return q.owner == owners[i]; })) continue;
            const float own = VolumeOf(owners[i]);
            if (own < 0.0f) continue;
            g_quiet.push_back({owners[i], roots[i], groups[i], own, -1.0f});
            ++g_quietSeen;
        }
    for (Quiet& q : g_quiet) {
        if (q.written >= 0.0f && std::fabs(VolumeOf(q.owner) - q.written) > 0.001f) ++g_quietOverwritten;
        q.written = float(q.volume * level);
        SetVolumeOf(q.owner, q.written);
    }
}

// Towards a level, at most step a call.
double Approach(double level, double target, double step) {
    return level < target ? std::min(target, level + step) : std::max(target, level - step);
}
}  // namespace

void Start(const std::vector<std::uint32_t>& clips, const std::string& track) {
    if (g_started) Stop();
    if (!CodeMatches()) return;
    // (closing's fades not done yet: done at once)
    if (g_musicStopping || OwnerOf(g_musicHandle)) EndMusic();
    if (!g_quiet.empty()) {
        QuietSounds(1.0, false);
        g_quiet.clear();
    }
    g_started = true;
    g_track = track;
    g_quickEnds = g_restarts = 0;
    g_musicGaveUp = false;
    g_voiceLogs = 0;
    g_musicLevel = 0.0;
    g_musicFadingIn = true;
    g_quietLevel = 1.0;
    g_musicOverwritten = g_quietOverwritten = g_quietSeen = g_ducks = 0;
    g_lastTick = Now();
    g_quietGroups.clear();
    if (!ReadGroups()) {
        Log::Write("ModelViewer: the game's sound groups could not be read; no music, the level's sounds play on");
        return;
    }
    // the relic lines' groups and the owner of ours: a relic line's DTP that is there
    g_voiceGroups.clear();
    g_ownerDtp = 0;
    int loaded = 0;
    for (const std::uint32_t clip : clips) {
        if (!clip || !DtpEdit::Loaded(clip)) continue;
        ++loaded;
        if (!g_ownerDtp) g_ownerDtp = clip;
        unsigned group = 0;
        if (DtpGroup(clip, group) && std::find(g_voiceGroups.begin(), g_voiceGroups.end(), group) == g_voiceGroups.end())
            g_voiceGroups.push_back(group);
    }
    char musicName[64], sliderName[64];
    std::string voices;
    for (const unsigned v : g_voiceGroups) {
        char n[64];
        voices += std::string(voices.empty() ? "" : ", ") + NameOf(v, n, sizeof(n));
    }
    Log::Writef("ModelViewer: sound: the music group %s (the music volume's %s); the relics' lines: %d of %zu there%s (their "
                "groups: %s)", NameOf(g_music1, musicName, sizeof(musicName)), NameOf(g_music2, sliderName, sizeof(sliderName)),
                loaded, clips.size(), DtpEdit::Available() ? "" : " (the game's DTP table is not available)",
                voices.empty() ? "none read" : voices.c_str());
    // (the music group, when it is under the music volume's: else that one)
    g_musicGroup = Under(g_music1, {g_music2}) ? g_music1 : g_music2;
    {   // the level's sounds now (the log: their groups)
        QuietSounds(1.0, true);
        std::vector<std::pair<unsigned, int>> counts;
        for (const Quiet& q : g_quiet) {
            auto it = std::find_if(counts.begin(), counts.end(), [&](const auto& c) { return c.first == q.group; });
            if (it == counts.end()) counts.push_back({q.group, 1});
            else ++it->second;
        }
        std::string what;
        for (std::size_t i = 0; i < counts.size() && i < 12; ++i) {
            char n[96];
            what += std::string(i ? ", " : "") + NameOf(counts[i].first, n, sizeof(n)) + " x" + std::to_string(counts[i].second);
        }
        Log::Writef("ModelViewer: the level's sounds faded out over %.1f s: %zu playing (%s); the environment's and the menu's "
                    "music, and any of theirs that start while open", kQuietOut, g_quiet.size(), what.empty() ? "none" : what.c_str());
    }
    if (track.empty()) {
        Log::Write("ModelViewer: no music ([ModelViewer] Music=Off)");
        return;
    }
    if (!g_ownerDtp) {
        Log::Write("ModelViewer: no music: none of the relics' lines (globaluiinfo's sound DTPs) is loaded to own it");
        return;
    }
    char groupName[64];
    NameOf(g_musicGroup, groupName, sizeof(groupName));
    const std::string key = track + "|" + std::to_string(g_musicGroup) + "|" + std::to_string(g_ownerDtp);
    if (!g_musicBuilt || g_musicBuiltFor != key) BuildMusic(track, g_musicGroup, g_ownerDtp);
    if (StartMusic("on opening"))
        Log::Detailf("ModelViewer: the music started, fading in over %.1f s: pcx64-w\\audio\\streams\\%s.mul (the %s group, owned "
                    "by DTP %u)", kMusicIn, track.c_str(), groupName, g_ownerDtp);
}

// (game thread, every frame: CutsceneViewer's tick) The fades; ours played again when it ended.
void Tick() {
    const double now = Now();
    const double dt = std::clamp(now - g_lastTick, 0.0, 0.1);
    g_lastTick = now;
    if (!g_started && !g_musicStopping && g_quiet.empty()) return;
    // a relic's line over: let go
    if (g_voiceHandle && !OwnerOf(g_voiceHandle)) ReleaseHandle(g_voiceHandle);
    const bool speaking = g_voiceHandle != nullptr;
    // ours: up to full (down under a line), down to nothing when closed
    if (std::uint8_t* owner = OwnerOf(g_musicHandle)) {
        const double target = !g_started ? 0.0 : speaking ? kDuckLevel : 1.0;
        double time;   // (s for the whole 0..1: the fade in and out as said, the duck's from full to its level and back)
        if (!g_started) time = kMusicOut;
        else if (target < g_musicLevel) time = kDuckDown / (1.0 - kDuckLevel);
        else time = g_musicFadingIn ? kMusicIn : kDuckUp / (1.0 - kDuckLevel);
        g_musicLevel = Approach(g_musicLevel, target, dt / time);
        if (g_musicLevel >= 1.0) g_musicFadingIn = false;
        if (g_musicWritten >= 0.0f && std::fabs(VolumeOf(owner) - g_musicWritten) > 0.001f) ++g_musicOverwritten;
        g_musicWritten = float(g_musicLevel);
        SetVolumeOf(owner, g_musicWritten);
        if (!g_started && g_musicLevel <= 0.0) {
            EndMusic();
            Log::Writef("ModelViewer: the music faded out and ended (its volume changed by the game %d times)", g_musicOverwritten);
        }
    } else if (g_musicStopping) {
        g_musicStopping = false;   // (it ended by itself while fading out)
        ReleaseHandle(g_musicHandle);
    }
    // the level's sounds: out while open, back after
    g_quietLevel = Approach(g_quietLevel, g_started ? 0.0 : 1.0, dt / (g_started ? kQuietOut : kQuietIn));
    if (!g_quiet.empty() || g_started) QuietSounds(g_quietLevel, g_started);
    if (!g_started && g_quietLevel >= 1.0 && !g_quiet.empty()) {
        Log::Writef("ModelViewer: the level's sounds back (%zu playing of the %d faded; their volumes changed by the game %d times)",
                    g_quiet.size(), g_quietSeen, g_quietOverwritten);
        g_quiet.clear();
    }
    // ours played again when it ended (a .mul without a loop)
    if (!g_started || g_musicGaveUp || !g_musicBuilt || g_track.empty() || !g_ownerDtp) return;
    if (OwnerOf(g_musicHandle)) return;
    const double lasted = now - g_musicStartedAt;
    if (lasted < 0.5) return;   // (a try each half second at most)
    ReleaseHandle(g_musicHandle);
    g_quickEnds = lasted < 3.0 ? g_quickEnds + 1 : 0;
    if (g_quickEnds >= 3) {
        g_musicGaveUp = true;
        Log::Writef("ModelViewer: the music ended within 3 s three times: given up (pcx64-w\\audio\\streams\\%s.mul not in the "
                    "game's archives?)", g_track.c_str());
        return;
    }
    ++g_restarts;
    const bool started = StartMusic("again");
    if (g_restarts <= 3 || !started)
        Log::Writef("ModelViewer: the music ended after %.1f s: played again%s", lasted, started ? "" : " (it did not start)");
}

void Stop() {
    if (!g_started) return;
    g_started = false;
    StopVoice();
    g_musicStopping = OwnerOf(g_musicHandle) != nullptr;   // (faded out, then ended: Tick)
    if (!g_musicStopping) ReleaseHandle(g_musicHandle);
    Log::Writef("ModelViewer: the viewer closed: the music fading out over %.1f s, the level's sounds back over %.1f s (the music "
                "lowered under %d relic lines)", kMusicOut, kQuietIn, g_ducks);
}

void PlayVoice(std::uint32_t clip, const char* what) {
    StopVoice();
    if (!g_started || !clip) return;
    if (!DtpEdit::Loaded(clip)) {
        if (g_voiceLogs++ < 8) Log::Writef("ModelViewer: %s's line (sound DTP %u) is not loaded: not played", what, clip);
        return;
    }
    g_voiceHandle = PlayDtp(clip);
    const bool playing = OwnerOf(g_voiceHandle) != nullptr;
    if (!playing) ReleaseHandle(g_voiceHandle);
    else ++g_ducks;
    if (g_voiceLogs++ < 8 || !playing)
        Log::Writef("ModelViewer: %s's line (sound DTP %u) %s", what, clip, playing ? "played (the music lowered under it)" : "did not start");
}

// A relic line's Assignment options and Stream (its root an Assignment over a Stream). SEH only.
bool RelicSettings(std::uint32_t id, Options& options, Stream& stream, float& volume, float& pitch) noexcept {
    const auto* plex = static_cast<const Plex*>(DtpEdit::DataPointer(id));
    if (!plex) return false;
    __try {
        if (plex->type != 6 || !plex->data) return false;
        const auto* assignment = static_cast<const Assignment*>(plex->data);
        if (!assignment->options || !assignment->sound || assignment->sound->type != 4 || !assignment->sound->data) return false;
        options = *assignment->options;
        stream = *static_cast<const Stream*>(assignment->sound->data);
        volume = assignment->volume;
        pitch = assignment->pitch;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void PlayVoiceStream(const std::string& name, const char* what) {
    StopVoice();
    if (!g_started || name.empty()) return;
    static std::map<std::string, Music*> voices;   // (one each, kept: the game may still be ending one)
    Music*& v = voices[name];
    if (!v) {
        Options options{};
        Stream stream{};
        float volume = 1.0f, pitch = 0.0f;
        if (!g_ownerDtp || !RelicSettings(g_ownerDtp, options, stream, volume, pitch)) {
            Log::Writef("ModelViewer: %s's line: no relic line's sound settings to play it with (none loaded)", what);
            return;
        }
        v = new Music{};
        strncpy_s(v->name, name.c_str(), _TRUNCATE);
        v->stream = stream;
        v->stream.name = v->name;
        v->stream.subtitleMode = 2;   // (never)
        v->streamPlex.type = 4;
        v->streamPlex.data = &v->stream;
        v->streamPlex.ownerId = g_ownerDtp;
        v->options = options;
        v->assignment.sound = &v->streamPlex;
        v->assignment.panning = v->panning;
        v->assignment.options = &v->options;
        v->assignment.volume = volume;
        v->assignment.pitch = pitch;
        v->root.type = 6;
        v->root.data = &v->assignment;
        v->root.ownerId = g_ownerDtp;
    }
    g_voiceHandle = PlayOurSound(&v->root);
    const bool playing = OwnerOf(g_voiceHandle) != nullptr;
    if (!playing) ReleaseHandle(g_voiceHandle);
    else ++g_ducks;
    Log::Writef("ModelViewer: %s's line pcx64-w\\audio\\streams\\%s.mul %s", what, name.c_str(),
                playing ? "played (the music lowered under it)" : "did not start");
}

bool VoicePlaying() { return OwnerOf(g_voiceHandle) != nullptr; }

void StopVoice() {
    if (!g_voiceHandle) return;
    EndOwner(OwnerOf(g_voiceHandle));   // (at once: the next relic's line after it, not over it)
    ReleaseHandle(g_voiceHandle);
}
}  // namespace ViewerAudio
