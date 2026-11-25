// The cutscene viewer: catalog and test panel (cutscenes.h).
#include "cutscenes.h"
#include "cutscene_titles.h"

#include <windows.h>
#include <intrin.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include <MinHook.h>

#include "features.h"
#include "game_archive.h"
#include "game_build.h"
#include "game_data.h"
#include "imgui.h"
#include "log.h"
#include "zone_list.h"
#include "nlohmann/json.hpp"

namespace Cutscenes {
namespace {
namespace fs = std::filesystem;
using GameArchive::ArchiveSet;
using GameArchive::Get;
using GameArchive::Lower;

// ---- the game (retail RVAs; TR2_x64_final.exe + PDB, matched by code) ----
// _NtCineImpl::Get(unitId, index) -> the cutscene's script object (made once, kept in _NtCineImpl::s_cineMap):
// +0 index, +4 its stream ID (-1: not created), +8 unit ID. (The unit comes first: a new object gets +0 = the second
// argument, +8 = the first, retail 0x1F85AF. 0.9.341's first tests passed them the other way round: the "unit" was the
// index, no level had that ID, nothing was created.)
constexpr std::uintptr_t kRvaCineGet = 0x001F84F0ull;
constexpr std::uint8_t kCineGetSig[] = {0x48, 0x89, 0x5C, 0x24, 0x10, 0x48, 0x89, 0x6C, 0x24, 0x20, 0x57, 0x48, 0x83,
                                        0xEC, 0x30, 0x4C, 0x8B, 0x1D, 0x92, 0x74, 0x85, 0x01, 0x33, 0xED};
// _NtCineImpl::Play(impl, create): EVENT_CreateCinematic(UnitData list[index], unitId) when not created, then
// EVENT_PlayCinematic(streamId).
constexpr std::uintptr_t kRvaCinePlay = 0x001FE470ull;
constexpr std::uint8_t kCinePlaySig[] = {0x48, 0x89, 0x5C, 0x24, 0x08, 0x57, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x8B, 0xD9,
                                         0x0F, 0xB6, 0xFA};
// _NtCineImpl::IsPlaying(impl): its stream's CinematicHandler is the current one and it plays.
constexpr std::uintptr_t kRvaCineIsPlaying = 0x001FCFD0ull;
constexpr std::uint8_t kCineIsPlayingSig[] = {0x40, 0x53, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x8B, 0xD9, 0x48, 0x8B, 0x0D,
                                              0xF8, 0xA9, 0xB5, 0x02};
// NtCinematic::Stop(NtCinematic*): (an NtCinematic is a pointer to the impl) stops and releases the stream.
constexpr std::uintptr_t kRvaCineStop = 0x00206700ull;
constexpr std::uint8_t kCineStopSig[] = {0x40, 0x53, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x8B, 0x19, 0x48, 0x85, 0xDB, 0x74,
                                         0x45, 0x48, 0x8B};
// NtCinematic::GetFrameNumber(NtCinematic*): the current CinematicHandler's frame (-1: none).
constexpr std::uintptr_t kRvaCineFrame = 0x001F8F00ull;
constexpr std::uint8_t kCineFrameSig[] = {0x48, 0x83, 0x39, 0x00, 0x74, 0x10, 0x48, 0x8B, 0x0D, 0xCB, 0xEA, 0xB5, 0x02,
                                          0x48, 0x85, 0xC9};
// STREAM_GetLevelWithID(unitId) -> Level (+0x30 UnitData: +0x68 cutscene count, +0x78 their names).
constexpr std::uintptr_t kRvaGetLevelWithId = 0x004215F0ull;
constexpr std::uint8_t kGetLevelWithIdSig[] = {0x4C, 0x8D, 0x15, 0xD9, 0xCC, 0x91, 0x02, 0x45, 0x33, 0xC0, 0x49, 0x8B,
                                               0xC2, 0x4C, 0x8D, 0x0D};
// The current CinematicHandler (global pointer) and CinematicHandler::IsCinematicPlaying(handler, bool).
constexpr std::uintptr_t kRvaCurrentCineHandler = 0x02D579D8ull;
constexpr std::uintptr_t kRvaHandlerIsPlaying = 0x00169D30ull;
constexpr std::uint8_t kHandlerIsPlayingSig[] = {0x80, 0xB9, 0x68, 0x03, 0x00, 0x00, 0x00, 0x74, 0x13, 0x83, 0x79, 0x40,
                                                 0xFF, 0x74, 0x0D, 0x84};
// CinematicHandler: +0x38 the cutscene's name (GetDebugCineName, 0x1694F0; the game's cloth / hair / inventory read
// it), +0x48 its frame position (float, GetFramePosition 0x169510).
constexpr std::size_t kHandlerNameOffset = 0x38, kHandlerFrameOffset = 0x48;
constexpr std::uint8_t kHandlerNameSig[] = {0x48, 0x8B, 0x51, 0x38, 0x48, 0x8D, 0x05};
constexpr std::uintptr_t kRvaHandlerName = 0x001694F0ull;
// NetManagerPC::Get and NetManager::ReturnToMainMenu(netManager, true) (what the PC benchmark calls when it is done).
constexpr std::uintptr_t kRvaNetManagerGet = 0x005817A0ull;
constexpr std::uint8_t kNetManagerGetSig[] = {0x48, 0x8B, 0x05, 0xA9, 0xA3, 0x80, 0x02, 0xC3};
constexpr std::uintptr_t kRvaReturnToMainMenu = 0x0058D8A0ull;
constexpr std::uint8_t kReturnToMainMenuSig[] = {0x48, 0x89, 0x5C, 0x24, 0x10, 0x56, 0x48, 0x83, 0xEC, 0x20, 0x0F, 0xB6,
                                                 0xF2, 0x48, 0x8B, 0xD9};
// NetManager::GetCurrGameState / GetCurrNetState (TR2 PDB, ??_7NetManager@@6B@ +0x80 / +0xA8; retail NetManagerPC's
// vtable 0xD57120 has them there, and ReturnToMainMenu calls +0x80, +0x90 GetNextGameState and +0x78 SetNextGameState
// as TR2's does): the game's GameStateID (2 menuing, 3 loading, 4 waiting, 5 playing, 6 resetting the intros, 7 quitting,
// 8 post-playing) and NetStateID (0 idle, 7 in a session, 8 leaving).
constexpr std::uintptr_t kRvaGetCurrGameState = 0x005818A0ull, kRvaGetCurrNetState = 0x005818B0ull;
constexpr std::uint8_t kGetCurrGameStateSig[] = {0x48, 0x83, 0xE9, 0x80, 0x48, 0x8B, 0x01, 0x48, 0xFF, 0x60, 0x28};
constexpr std::uint8_t kGetCurrNetStateSig[] = {0x48, 0x8B, 0x41, 0x10, 0x48, 0x83, 0xC1, 0x10, 0x48, 0xFF, 0x60, 0x28};
// StreamTracker: 32 StreamUnit of 0x2A0 bytes (+0 ID, +4 state: low 7 bits, 0 free, 4 being dumped; +0x18 name).
constexpr std::uintptr_t kRvaStreamTracker = 0x02D3E2D0ull;
constexpr std::size_t kUnitStride = 0x2A0, kUnitNameOffset = 0x18;
constexpr int kUnitCount = 32;
constexpr std::size_t kLevelUnitDataOffset = 0x30, kUnitDataCineCountOffset = 0x68, kUnitDataCineListOffset = 0x78;

using CineGetFn = void*(__fastcall*)(std::uint32_t, std::uint32_t);
using CinePlayFn = bool(__fastcall*)(void*, bool);
using CineIsPlayingFn = bool(__fastcall*)(void*);
using CineStopFn = void(__fastcall*)(void**);
using CineFrameFn = int(__fastcall*)(void**);
using GetLevelFn = void*(__fastcall*)(int);
using HandlerIsPlayingFn = bool(__fastcall*)(void*, bool);

bool g_gameReady = false;
CineGetFn g_cineGet = nullptr;
CinePlayFn g_cinePlay = nullptr;
CineIsPlayingFn g_cineIsPlaying = nullptr;
CineStopFn g_cineStop = nullptr;
CineFrameFn g_cineFrame = nullptr;
GetLevelFn g_getLevel = nullptr;
HandlerIsPlayingFn g_handlerIsPlaying = nullptr;

// ---- the catalog ----
struct Entry {
    std::string id;           // the cutscene's name (or the video's file name without .bik)
    bool video = false;       // a pre-rendered video (movies\pc)
    std::string unit;         // the unit it plays in (from its stream's header)
    std::string archive;      // the archive holding it
    std::string title, category, description, image;
    // the title and description in the game's other text languages (cutscenes.json "titles", "descriptions": its
    // language code -> the text), ReadTranslations
    std::map<std::string, std::string> titles, descriptions;
    bool hidden = false;
    // how the game reaches it: a Chapter Select entry (unit + runnable) whose story plays it (cutscenes.json "start";
    // when the file has none, matched from the game's "Cinematic: ..." / "PCAP: NN ..." entries)
    std::string startUnit, startRunnable;
    bool startFromFile = false;
    // "node": after the start the viewer fires the level's cutscene node (a checkpoint before it); "load": the level
    // plays it by itself while or right after loading. Learned when it plays (cutscenes.json start.trigger).
    std::string trigger;
    // a cutscene of the same story that leads to it (cutscenes.json start.via): when the level loaded from the start has
    // no node for this one (it plays in a unit the story loads next: Execution after The Reckoning), the viewer fires
    // that one's node instead; it is skipped as the player would, and the story goes on to ours.
    std::string via;
    // objects whose instances are hidden while the viewer plays it (cutscenes.json "hide": object names): a gameplay
    // copy of a prop the story hides before the cutscene (Empty Tomb: bp_sarcophagus_01 beside the cutscene's
    // bp_prologue_sarcophagus_rig).
    std::vector<std::string> hide;
    // objects whose instances are removed from the level once it is loaded (cutscenes.json "remove": object names; the
    // game's Instance::EnableRemoveThisFrame): what the viewer's start has running that the story has stopped by then.
    // In Through the Out Door: fxt_puzzle_tremors, the tremors' falling rocks (its sounds: stone_rock_large_impact_*,
    // stone_rocks_small_debris_*), all through the scene from Sauna Fight (the user, 0.9.391 to 0.9.395: visible rocks
    // falling, loud; not in the story's)
    std::vector<std::string> remove;
    // stream layers of its unit turned on before it, beside its own props layer (cutscenes.json "layers": layer names,
    // as the log lists them): the area's content the player's way there would have turned on (A Connection plays in
    // the truck depot; with Content_Truck_Depot off it stood empty, 0.9.362).
    std::vector<std::string> layers;
    // stream layers of its unit turned off before it (cutscenes.json "layersOff"), after "layers" are on: what the story
    // switches off on the way to it that its start has on (Whispers in the Dark: the first dusk's light and deep snow,
    // 0.9.373's log of the story)
    std::vector<std::string> layersOff;
    // stream layers turned off as the game starts it (cutscenes.json "layersOffAtStart"; "<unit>:<layer>", or a layer of
    // the start's unit): what the story switches off between the scene's node and its first frame. Turned off before
    // the node fires they could take the graph that plays it with them. In Through the Out Door: the Ice Tomb's
    // content (0.9.391's log of the story; Lara stood in its rocks in the viewer)
    std::vector<std::string> layersOffAtStart;
    // other units streamed in before it (cutscenes.json "units": unit names), as the story has them loaded when it
    // plays (Locating the Source: sh_soviet_hub, 0.9.376's log of the story). A "layers" / "layersOff" name
    // "<unit>:<layer>" is a layer of another loaded unit.
    std::vector<std::string> units;
    // the cutscenes the story plays right after it, as one scene with it (cutscenes.json "then": Revealing the Path, then
    // A Choice): the viewer goes on to them before it goes back to the menu, and they are not listed on their own.
    std::vector<std::string> then;
    // the story's event its way to the scene starts from (cutscenes.json "storyEvent": "<node>:<output>", a node of the
    // scene's graph that relays another instance's graph, AGInstance_ActionGraphInstanceNode, and the output of that
    // graph): the viewer sends it to every node that relays that graph, as the game does when the graph sends it, and
    // the scene comes the story's way (Retreat: the mini-boss fight's end; until then its explosions and their camera
    // shake go on, 0.9.381). Before it, the level's delays running in the graphs it reaches are run out; a delay right
    // after the scene's own relay passes at once (0.9.385). "" for none.
    std::string storyEvent;
    // while the viewer plays it: the level's NPCs that are not its actors hidden (cutscenes.json "hideNpcs": To the
    // Tower's checkpoint has the village's people walking about, 0.9.364); the level's music kept ("keepLevelMusic":
    // otherwise the music playing before its node fires is ended, EndLevelMusic)
    bool hideNpcs = false, keepLevelMusic = false;
};

std::mutex g_catalogMutex;
std::vector<Entry> g_catalog;
std::atomic<int> g_scanState{0};   // 0 not started, 1 scanning, 2 done, 3 failed
std::string g_scanStatus = "not started";

fs::path GameRoot() {
    wchar_t path[MAX_PATH]{};
    const DWORD len = GetModuleFileNameW(nullptr, path, MAX_PATH);
    if (!len || len >= MAX_PATH) return fs::current_path();
    return fs::path(path).parent_path();
}

std::string MulPath(const std::string& name) { return "pcx64-w\\cinstream\\" + Lower(name) + ".mul"; }

std::string CString(const std::vector<std::uint8_t>& b, std::size_t at) {
    std::string s;
    while (at < b.size() && b[at] && s.size() < 512) s.push_back(char(b[at++]));
    return s;
}

bool Identifier(const std::string& s) {
    if (s.empty()) return false;
    for (const char c : s)
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_')) return false;
    return true;
}

// The unit a cutscene plays in: its CINE stream's header ("ENIC", then the unit's name), in the first type-1 block.
std::string UnitOf(ArchiveSet& set, const std::string& name) {
    constexpr std::size_t kMulHeader = 0x2000;
    for (const auto& copy : set.Copies(MulPath(name))) {
        if (!copy.usable) continue;
        std::vector<std::uint8_t> head;
        if (!set.ReadPrefix(copy.location(), 0x8000, head) || head.size() < kMulHeader + 16) return {};
        for (std::size_t at = kMulHeader; at + 16 <= head.size();) {
            const std::uint32_t type = Get<std::uint32_t>(head, at), size = Get<std::uint32_t>(head, at + 4);
            if (!type && !size) {
                at += 16;
                continue;
            }
            if (type == 1) {
                const std::size_t p = at + 16;
                if (p + 16 + 64 <= head.size() && !std::memcmp(head.data() + p, "ENIC", 4)) return Lower(CString(head, p + 16));
                return {};
            }
            at = (at + 16 + size + 15) & ~std::size_t(15);
        }
        return {};
    }
    return {};
}

std::string ArchiveOf(ArchiveSet& set, const std::string& path) {
    for (const auto& copy : set.Copies(path))
        if (copy.usable && copy.archive) return copy.archive->name;
    return {};
}

// Every cutscene name (as the Suite's Cine::FindNames).
std::vector<std::string> FindNames(ArchiveSet& set, const std::map<std::uint32_t, std::string>& zoneNames) {
    std::set<std::string> candidates;
    {
        wchar_t exe[MAX_PATH]{};
        GetModuleFileNameW(nullptr, exe, MAX_PATH);
        std::ifstream in(exe, std::ios::binary);
        std::string run;
        char buffer[1 << 16];
        while (in.read(buffer, sizeof(buffer)) || in.gcount() > 0) {
            const std::streamsize n = in.gcount();
            for (std::streamsize i = 0; i < n; ++i) {
                const char c = buffer[i];
                if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_') {
                    run.push_back(c);
                    continue;
                }
                if (c == 0 && run.size() >= 4 && run.size() < 64) candidates.insert(run);
                run.clear();
            }
        }
    }
    for (const auto& [id, name] : zoneNames) candidates.insert(Lower(name));
    // streams no scanned source names: The Deathless Ones plays in fa_020_archive, a unit without another cutscene in
    // the catalog (never scanned), and its name is neither in ROTTR.exe nor an object's (0.9.389; the story plays it
    // from "Flooded Archive: Atlas Vista - Pre Reveal" in 0.9.388's log)
    for (const char* name : {"46_thedeathlessones_cine"}) candidates.insert(name);
    std::set<std::string> names;
    std::vector<std::string> units;
    for (const auto& c : candidates) {
        if (set.HasFile(MulPath(c))) names.insert(c);
        else if (c.find("cine") != std::string::npos && set.HasFile("pcx64-w\\" + c + ".drm")) units.push_back(c);
    }
    for (const auto& name : names) units.push_back(UnitOf(set, name));
    std::set<std::string> scanned;
    for (std::size_t u = 0; u < units.size(); ++u) {
        const std::string unit = units[u];
        if (unit.empty() || !scanned.insert(unit).second) continue;
        std::vector<std::uint8_t> bytes;
        GameArchive::Drm drm;
        if (!set.ReadFile("pcx64-w\\" + unit + ".drm", bytes) || !drm.Parse(bytes)) continue;
        std::set<std::int32_t> seen;
        for (std::size_t i = 0; i < drm.ids.size(); ++i) {
            if (drm.ids[i].type != GameArchive::kTypeDtp || !seen.insert(drm.ids[i].id).second) continue;
            std::vector<std::uint8_t> data;
            if (!set.ReadBlob(drm.LocationOf(i), data, drm.TotalSize(i))) continue;
            const std::uint64_t refs = GameArchive::RefDefinitionsSize(data);
            if (refs > data.size()) continue;
            std::map<std::uint32_t, std::uint32_t> internal;
            const std::uint32_t internals = Get<std::uint32_t>(data, 0);
            for (std::uint32_t k = 0; k < internals; ++k)
                internal[Get<std::uint32_t>(data, 20 + k * 8)] = Get<std::uint32_t>(data, 20 + k * 8 + 4);
            const std::vector<std::uint8_t> body(data.begin() + std::ptrdiff_t(refs), data.end());
            if (body.size() < 2304) continue;
            const auto list = internal.find(0x78), own = internal.find(0);
            if (list == internal.end() || own == internal.end()) continue;
            const std::uint32_t count = Get<std::uint32_t>(body, 0x68);
            if (!count || count >= 1000) continue;
            const bool unitData = Lower(CString(body, own->second)) == unit;
            for (std::uint32_t k = 0; k < count; ++k) {
                const auto t = internal.find(list->second + 8 * k);
                if (t == internal.end()) continue;
                const std::string name = Lower(CString(body, t->second));
                if (Identifier(name) && !names.count(name) && set.HasFile(MulPath(name))) {
                    names.insert(name);
                    units.push_back(UnitOf(set, name));
                }
            }
            if (unitData) break;   // (the unit's own UnitData: done with this unit)
        }
    }
    return {names.begin(), names.end()};
}

// Default titles: the game's names are the scenes' titles without spaces.
const std::map<std::string, const char*>& KnownTitles() {
    static const std::map<std::string, const char*> titles = {
        {"010_nadia_cine", "Nadia"},
        {"01_rockbottom_cine", "Rock Bottom"},
        {"02_confessions_cine", "Confessions"},
        {"030_the_antidote_cine", "The Antidote"},
        {"03_carcrash_cine_03_part01", "Car Crash"},
        {"04_thisistheplace_cine", "This Is the Place"},
        {"08_emptytomb_cine", "Empty Tomb"},
        {"09_ucl_cine", "UCL"},
        {"10_revelations_cine", "Revelations"},
        {"11_suppression_cine", "Suppression"},
        {"14_expedtion_a_cine", "Long Journey"},   // (the game's opening: Chapter Select's "Cinematic: Long Journey")
        {"15_almostthere_cine", "Almost There"},
        {"20_alive_cine", "Alive"},
        {"21_whispersinthedark_cine", "Whispers in the Dark"},
        {"22a_theenemy_cine", "The Enemy"},
        {"22b_sofia_cine", "Sofia"},
        {"23_aconnection_cine", "A Connection"},
        {"25_captured_cine", "Captured"},
        {"26_ana_cine", "Ana"},
        {"27_jacob_cine", "Jacob"},
        {"27b_trustissues_cine", "Trust Issues"},
        {"27c_waithere_cine", "Wait Here"},
        {"28_atlas_cine", "The Atlas"},
        {"28b_getoutofdodge_cine", "Get Out of Dodge"},
        {"29_theplunge_cine", "The Plunge"},
        {"30_thedrowning_cine_b", "The Drowning"},
        {"31a_aloneagain_cine", "Alone Again"},
        {"31b_findingjacob_cine", "Finding Jacob"},
        {"35_inthroughtheoutdoor_cine", "In Through the Out Door"},
        {"36_meettheremnant_cine", "Meet the Remnant"},
        {"38_signalfire_cine", "Signal Fire"},
        {"39_shotgun_cine", "Shotgun"},
        {"39b_setback_cine", "Setback"},
        {"40_tothetower_cine", "To the Tower"},
        {"40b_courtyardincursion_cine", "Courtyard Incursion"},
        {"40c_remnantresistance_cine", "Remnant Resistance"},
        {"40d_planofaction_cine", "Plan of Action"},
        {"41_retreat_cine", "Retreat"},
        {"44_cathedralambush_cine", "Cathedral Ambush"},
        {"45_trapped_cine", "Trapped"},
        {"46_thedeathlessones_cine", "The Deathless Ones"},
        {"47b_reunion_cine", "Reunion"},
        {"47c_revealingthepath_cine", "Revealing the Path"},
        {"47d_achoice_cine", "A Choice"},
        {"47e_regrets_cine", "Regrets"},
        {"48_confrontations_cine", "Confrontations"},
        {"48a_locatingthesource_cine", "Locating the Source"},
        {"50_anoldfriend_cine", "An Old Friend"},
        {"50b_jonahsdemise_cine", "Jonah's Demise"},
        {"51_tehrace_cine", "The Race"},
        {"56_theconstanceofkonstantin_cine", "The Constance of Konstantin"},
        {"57_thereckoning_cine", "The Reckoning"},
        {"57b_execution_cine", "Execution"},
        {"59_anewbeginning_cine", "A New Beginning"},
        {"60_aftermath_cine", "Aftermath"},
        {"av_avalanche_spire_attach", "Avalanche: The Spire"},
        {"benchmark_avalanche_circle", "Benchmark: Avalanche"},
        {"benchmark_bp_cine_scene", "Benchmark: Black Sea"},
        {"downpour_lara_climb_up", "Downpour: Climbing Up"},
        {"flythrough_vh3_day", "Flythrough: Geothermal Valley"},
        {"gesc_bossintro", "Gulag Escape: Boss"},
        {"gesc_doorshut", "Gulag Escape: The Door"},
        {"gesc_ltrsurprise", "Gulag Escape: Surprise"},
        {"gesc_puzzle_pickup", "Gulag Escape: The Pickup"},
        {"gesc_shrapnelgrenadeintro", "Gulag Escape: Shrapnel Grenades"},
        {"hunting_ground_cine", "Hunting Ground"},
        {"iceclimb_collapse_reaction", "Ice Climb: Collapse"},
        {"iceclimb_jump_success", "Ice Climb: The Jump"},
        {"iceclimb_outro_long", "Ice Climb: Outro"},
        {"iceclimb_outro_short", "Ice Climb: Outro (Short)"},
        {"igc_acquirecanteen", "The Canteen"},
        {"igc_delugefinale_cine", "Deluge Finale"},
        {"igc_eliteintro", "Trinity Elite"},
        {"igc_fa_rambo_tunnel_pyro", "Flooded Archive: The Tunnel"},
        {"igc_heli_pressure", "Helicopter"},
        {"igc_investigatetracks", "Investigating the Tracks"},
        {"igc_kh_top_of_acropolis", "Kitezh: The Acropolis"},
        {"igc_kitezh_hub_trebuchet_2", "Kitezh: The Trebuchet"},
        {"igc_kobk_firefly_outro", "Firefly"},
        {"igc_newgun_cine", "A New Gun"},
        {"igc_sh_regrets_outro", "Regrets (Outro)"},
        {"igc_shieldlooterentrance_part2", "Shield Looter"},
        {"igc_silent_night_establishing_shot", "Silent Night"},
        {"igc_vh_silent_night_trinity_elite_intro", "Silent Night: Trinity Elite"},
        {"kill_or_be_killed_flamethrower_intro", "Kill or Be Killed: Flamethrower"},
        {"lara_ledge_catch_pullup", "Ledge Catch"},
        {"live_cd_levelcomplete", "Cold Darkness: Complete"},
        {"rruins_enforcers_arrival", "Remnant Ruins: Enforcers"},
        {"rruins_grenadearrowpickup", "Remnant Ruins: Grenade Arrows"},
        {"rruins_helicoptercrash", "Remnant Ruins: Helicopter Crash"},
        {"truckpuzzlesolve", "The Truck"},
        // the videos
        {"scene_3b_good_times", "Good Times"},
        {"scene_20_alive_flashback", "Alive (Flashback)"},
        {"scene_30_the_drowning", "The Drowning (Video)"},
        {"scene_39b_est_shot", "Setback (Establishing Shot)"},
        {"scene_47e_regrets", "Regrets (Video)"},
        {"outro_manor_full", "Croft Manor (Outro)"},
        {"croft_manor_credits", "Credits"},
        {"bp_040_deluge", "The Deluge"},
    };
    return titles;
}

// Otherwise: the name without its scene number and "_cine", its words capitalized.
std::string GeneratedTitle(const std::string& id) {
    std::string s = Lower(id);
    for (const char* prefix : {"scene_", "igc_"})
        if (s.rfind(prefix, 0) == 0) s = s.substr(std::strlen(prefix));
    std::size_t p = 0;
    while (p < s.size() && std::isdigit(static_cast<unsigned char>(s[p]))) ++p;
    if (p > 0 && p + 1 < s.size() && std::isalpha(static_cast<unsigned char>(s[p])) && s[p + 1] == '_') ++p;
    if (p > 0 && p < s.size() && s[p] == '_') s = s.substr(p + 1);
    for (const char* suffix : {"_cine"})
        if (const auto at = s.rfind(suffix); at != std::string::npos && at + std::strlen(suffix) == s.size()) s.resize(at);
    std::string out;
    bool start = true;
    for (const char c : s) {
        if (c == '_') {
            out.push_back(' ');
            start = true;
            continue;
        }
        out.push_back(start ? char(std::toupper(static_cast<unsigned char>(c))) : c);
        start = false;
    }
    return out.empty() ? id : out;
}
std::string DefaultTitle(const std::string& id) {
    const auto& known = KnownTitles();
    if (auto it = known.find(Lower(id)); it != known.end()) return it->second;
    return GeneratedTitle(id);
}
// The title cutscenes.json gives: a generated one written by an earlier version ("Thereckoning", "Expedtion A") gives
// way to a known title; one written by hand stays.
std::string FileTitle(const nlohmann::json& item, const std::string& id, const std::string& fallback) {
    std::string title = item.value("title", fallback);
    const auto& known = KnownTitles();
    if (auto it = known.find(Lower(id)); it != known.end() && title == GeneratedTitle(id)) title = it->second;
    return title;
}

std::string CategoryOf(const std::string& id, const std::string& archive) {
    const std::string a = Lower(archive), n = Lower(id);
    if (a.find("babayaga") != std::string::npos || n.find("ivan") != std::string::npos ||
        n.find("babayaga") != std::string::npos)
        return "Baba Yaga";
    if (a.find("bloodties") != std::string::npos) return "Blood Ties";
    if (a.find("colddarkness") != std::string::npos || n.rfind("live_cd", 0) == 0) return "Cold Darkness";
    return "Story";
}

// Story order: the scene number (and its letter) first, then the name.
// Cutscenes the game lists but never plays (cut from it): never in the viewer's list. Long Journey: av_000_walk_talk
// lists 13_longjourney_cine, but its Chapter Select entry ("Cinematic: Long Journey") plays 14_expedtion_a_cine, the
// game's real opening scene; 13 played only when the viewer started it directly (0.9.361).
bool CutFromGame(const std::string& id) { return _stricmp(id.c_str(), "13_longjourney_cine") == 0; }
// Left out of the viewer's list whatever cutscenes.json says (v0.9.473, the user's list): Separation.
bool LeftOutOfViewer(const std::string& id) {
    return CutFromGame(id) || _stricmp(id.c_str(), "19_separation_cine") == 0;
}

std::pair<int, std::string> SortKey(const Entry& e) {
    std::string s = Lower(e.id);
    if (s.rfind("scene_", 0) == 0) s = s.substr(6);
    int number = 100000;
    std::size_t p = 0;
    while (p < s.size() && std::isdigit(static_cast<unsigned char>(s[p]))) ++p;
    if (p > 0) number = std::atoi(s.substr(0, p).c_str()) * 10;
    if (s == "57b_execution_cine") number = 595;   // (after the end credits, so after A New Beginning)
    return {number, s};
}

fs::path CatalogPath() { return GameRoot() / L"ROTTRFramework" / L"cutscenes" / L"cutscenes.json"; }

void ReadHideList(const nlohmann::json& item, std::vector<std::string>& hide) {
    hide.clear();
    if (!item.contains("hide") || !item["hide"].is_array()) return;
    for (const auto& name : item["hide"])
        if (name.is_string() && !name.get<std::string>().empty()) hide.push_back(name.get<std::string>());
}

// Objects known to be removed (Entry::remove; written into cutscenes.json while it has no "remove" for the cutscene).
const std::map<std::string, std::vector<std::string>>& KnownRemove() {
    static const std::map<std::string, std::vector<std::string>> remove = {
        {"35_inthroughtheoutdoor_cine", {"fxt_puzzle_tremors"}},
    };
    return remove;
}
void ReadRemoveList(const nlohmann::json& item, const std::string& id, std::vector<std::string>& remove) {
    remove.clear();
    if (!item.contains("remove") || !item["remove"].is_array()) {
        if (auto it = KnownRemove().find(Lower(id)); it != KnownRemove().end()) remove = it->second;
        return;
    }
    for (const auto& name : item["remove"])
        if (name.is_string() && !name.get<std::string>().empty()) remove.push_back(name.get<std::string>());
}

// Layers known to be needed (written into cutscenes.json while it has no "layers" for the cutscene). A Connection's was
// seen in game (0.9.363); the rest are the area layers its start leaves off, chosen from the unit's list
// (cutscenes\layers\<unit>.txt of 0.9.363), not seen in game yet:
// - Remnant Resistance, Plan of Action (br_010_outer_city, Courtyard Infiltration): its start has the valley's, the
//   courtyard's and the ruins' Art/Combat/Content layers on; Art_Ruins_Outside and Content_Ruins_Outside are the only
//   area layers off.
// - Revealing the Path (vh_valley_hub, VH3: Upper Ridge): Reunion's props (Cine_Props_Scene_47B), off at that start; the
//   two scenes follow each other in the story (A Choice's props are on there already). It plays in the observatory
//   (its dome falls in it: State_Change_Observatory_Dome_Intact / _Destroyed), whose floor was missing in 0.9.367: of
//   the observatory's layers only Terrain_Observatory_Structure, _Bear_Cave, _Bear_Arena and Content_Observatory_Bear_
//   Arena are on at that start; the unit's streaming regions (streaming_observatory_entrance, _ascent_trail,
//   _interior) turn the rest on as Lara gets there: Terrain_ and Content_Observatory_Entrance, _Ascent,
//   Content_Observatory_Interior.
// - Whispers in the Dark (bv_bear_valley, PCAP: 21 Whispers in the Dark, bv_021_branches_acquired): what the story
//   switched between that start and the scene (0.9.373's log, the level played to it): TOD_Dusk_Story_B and
//   VFX_Dusk_Story_B on, TOD_Dusk_Story_A and VFX_Dusk_Story_A off (KnownLayersOff), Snow_Terrain_Normal on and
//   Snow_Terrain_Deep off, cine_21_skydome on (and igc_bv_stumble on and off, Story_Triggers_1 off and on: the shelter's
//   building). Cine_Whispers_fx (here from 0.9.368 to 0.9.372) the story leaves off; turned on, the game turned it off
//   again as the scene's node fired (StreamLayerManager::DeactivateStreamLayer).
// - Locating the Source (gr_connector_to_sh, Safe Landing - Cliff): the unit's two cine layers, Cine_48_Props and
//   Cine_48_Props_delayed. Both were on at the cave's checkpoint (Safe Landing - Start, 0.9.368: its actors there); from
//   the cliff's (0.9.371) its 4 actors were MISSING, the stream stood in state 3 and it never came, as A Connection
//   without its props layer (0.9.360).
const std::map<std::string, std::vector<std::string>>& KnownLayers() {
    static const std::map<std::string, std::vector<std::string>> layers = {
        {"21_whispersinthedark_cine", {"TOD_Dusk_Story_B", "VFX_Dusk_Story_B", "Snow_Terrain_Normal", "cine_21_skydome"}},
        {"23_aconnection_cine", {"Content_Truck_Depot"}},
        {"40c_remnantresistance_cine", {"Art_Ruins_Outside", "Content_Ruins_Outside"}},
        {"40d_planofaction_cine", {"Art_Ruins_Outside", "Content_Ruins_Outside"}},
        {"48a_locatingthesource_cine", {"Cine_48_Props", "Cine_48_Props_delayed"}},   // (and KnownLayersOff, KnownUnits)
        // from Sauna Fight, the prayer room's (it_040_prayer_room, loaded with the Ice Tomb) terrain: on in the story
        // from the run out's start to the scene, off at Sauna Fight (0.9.391's log of the story, 0.9.389's layer
        // files). Its MainPath and Content_Facade the story has off at the scene's start (on in 0.9.390 and 0.9.391,
        // then Lara clipped through rocks); the scene turns Content_Facade on as it plays. KnownLayersOff,
        // KnownLayersOffAtStart
        {"35_inthroughtheoutdoor_cine", {"it_040_prayer_room:Prayer_Room_Terrain"}},
        // Reunion's props (off at VH3's start, where 47c needs them too: 0.9.363's layer files)
        {"47b_reunion_cine", {"Cine_Props_Scene_47B"}},
        {"47c_revealingthepath_cine",
         {"Cine_Props_Scene_47B", "Terrain_Observatory_Entrance", "Terrain_Observatory_Ascent",
          "Content_Observatory_Entrance", "Content_Observatory_Ascent", "Content_Observatory_Interior"}},
    };
    return layers;
}

// Layers known to be switched off by the story before the scene (Entry::layersOff; written into cutscenes.json while it
// has no "layersOff" for the cutscene): see KnownLayers.
const std::map<std::string, std::vector<std::string>>& KnownLayersOff() {
    static const std::map<std::string, std::vector<std::string>> layers = {
        {"21_whispersinthedark_cine", {"TOD_Dusk_Story_A", "VFX_Dusk_Story_A", "Snow_Terrain_Deep"}},
        // the story turns gr_010_gulag_rescue's trees off on the way (0.9.375's log, from Safe Landing - Start; the
        // scene's graph has a layer node for them), and they were off when it played (0.9.376)
        {"48a_locatingthesource_cine", {"gr_010_gulag_rescue:Content_Trees"}},
        // from Sauna Fight: off in the story from the run out's start, on at Sauna Fight (KnownLayers)
        {"35_inthroughtheoutdoor_cine", {"it_030_tomb:runout_remove_content"}},
    };
    return layers;
}

// Layers known to be switched off by the story as the scene starts (Entry::layersOffAtStart; written into
// cutscenes.json while it has no "layersOffAtStart" for the cutscene).
// - In Through the Out Door (0.9.391's log of the story, from "Run Out"): after the scene's node, before its first
//   frame, the game turned off it_030_tomb's Main_Game_Content, runout_only_content, Story_Playthrough,
//   zone_grandentrance_column_full and zone_truckBridge_section1 to 3: at the scene's start only Map_Culling and
//   cave_walls were on (the scene turns cave_walls off as it plays).
const std::map<std::string, std::vector<std::string>>& KnownLayersOffAtStart() {
    static const std::map<std::string, std::vector<std::string>> layers = {
        {"35_inthroughtheoutdoor_cine",
         {"it_030_tomb:Main_Game_Content", "it_030_tomb:runout_only_content", "it_030_tomb:Story_Playthrough",
          "it_030_tomb:zone_grandentrance_column_full", "it_030_tomb:zone_truckBridge_section1",
          "it_030_tomb:zone_truckBridge_section2", "it_030_tomb:zone_truckBridge_section3"}},
    };
    return layers;
}

// Units known to be loaded when the story plays the scene (Entry::units; written into cutscenes.json while it has no
// "units" for the cutscene). Locating the Source (0.9.376, the units at its start): from Safe Landing - Cliff the
// viewer had gr_connector_to_sh, gr_010_gulag_rescue, gr_global_gulag_rescue and sh_connector_to_gr loaded, the story
// (from Safe Landing - Start, walked to the scene) those and sh_soviet_hub; the layers on in the four were the same but
// for gr_010's Content_Trees. The viewer's was white around its actors, the story's in its dome.
const std::map<std::string, std::vector<std::string>>& KnownUnits() {
    static const std::map<std::string, std::vector<std::string>> units = {
        {"48a_locatingthesource_cine", {"sh_soviet_hub"}},
    };
    return units;
}

// Cutscenes the story plays right after another, as one scene (Entry::then; written into cutscenes.json while it has no
// "then" for the cutscene).
const std::map<std::string, std::vector<std::string>>& KnownChains() {
    static const std::map<std::string, std::vector<std::string>> chains = {
        {"47c_revealingthepath_cine", {"47d_achoice_cine"}},
    };
    return chains;
}
// Built-in "hideNpcs" (cutscenes.json says otherwise when it has it): seen in game (0.9.364: To the Tower's people,
// Retreat's fighters).
// Story events known (Entry::storyEvent; written into cutscenes.json while it has no "storyEvent" for the cutscene).
// Retreat (0.9.379's trace of the story, played from br_050_Tower_500_TowerClimb_Start through the mini-boss fight): the
// scene's graph's node 49 (an AGInstance_ActionGraphInstanceNode relaying the "Mini Boss - Spawn" graph) sent output 8
// 5 s before the scene; then node 50 (AGTimeDelayNode) -> 51 (AGLogicAnyNode) -> 3 (AGGraphicsOverrideNode) -> 10
// (SetAction, the player) and 0 (AGInstanceTeleportNode, the player) -> 33 (MiscPersistentDataNode, output 2) -> 34
// (Logic_PassThrough) -> 36 (AGTimeDelayNode) -> Play. 0.9.381's trace of the viewer's: the explosions' camera shake
// is a 19-node graph's (an AGMathRandomNumber of 8 to 12 s, an AGTimeStopwatch, the viewport_shake scriptable node, an
// AGSoundPlexNode), whose node 0 relays the same graph: its output 8 goes to the stopwatch's input 2.
const std::map<std::string, std::string>& KnownStoryEvents() {
    static const std::map<std::string, std::string> events = {
        {"41_retreat_cine", "49:8"},
    };
    return events;
}

void ReadStoryEvent(const nlohmann::json& item, Entry& e) {
    if (item.contains("storyEvent") && item["storyEvent"].is_string()) {
        e.storyEvent = item["storyEvent"].get<std::string>();
        return;
    }
    const auto& known = KnownStoryEvents();
    const auto it = known.find(Lower(e.id));
    e.storyEvent = it != known.end() ? it->second : std::string();
}

bool KnownHideNpcs(const std::string& id) {
    const std::string l = Lower(id);
    return l == "40_tothetower_cine" || l == "41_retreat_cine";
}
void ReadFlags(const nlohmann::json& item, Entry& e) {
    e.hideNpcs = item.contains("hideNpcs") && item["hideNpcs"].is_boolean() ? item["hideNpcs"].get<bool>() : KnownHideNpcs(e.id);
    e.keepLevelMusic =
        item.contains("keepLevelMusic") && item["keepLevelMusic"].is_boolean() && item["keepLevelMusic"].get<bool>();
}

// cutscenes.json "titles" and "descriptions": the title and description in the game's other text languages, by the
// game's language code (localstr_get_language_code: en, fr, de, it, es-mx, es, ja, pt, pl, ru, nl, ko, cht, chs, ar);
// the viewer shows the one of the game's language, else title / description. Without "titles", the built-in
// translations of the title (cutscene_titles.cpp) while it is the English one they translate.
void ReadTranslations(const nlohmann::json& item, Entry& e) {
    auto read = [&](const char* key, std::map<std::string, std::string>& out) {
        out.clear();
        if (!item.contains(key) || !item[key].is_object()) return false;
        for (const auto& [code, text] : item[key].items())
            if (text.is_string() && !text.get<std::string>().empty()) out[Lower(code)] = text.get<std::string>();
        return true;
    };
    if (!read("titles", e.titles))
        if (const auto* known = CutsceneTitles::Translations(e.id, e.title)) e.titles = *known;
    read("descriptions", e.descriptions);
}

void ReadThenList(const nlohmann::json& item, const std::string& id, std::vector<std::string>& then) {
    then.clear();
    if (!item.contains("then") || !item["then"].is_array()) {
        if (auto it = KnownChains().find(Lower(id)); it != KnownChains().end()) then = it->second;
        return;
    }
    for (const auto& name : item["then"])
        if (name.is_string() && !name.get<std::string>().empty()) then.push_back(name.get<std::string>());
}

// How each story cutscene came when the viewer played it (cutscenes.json start.trigger, learned in game up to 0.9.363):
// known before its first play, so that the chosen outfit is put on the right way the first time (a cutscene the level
// plays while it loads needs the outfit before the load; 0.9.363's first Cathedral Ambush and Aftermath came in the
// story's outfit, their trigger not learned yet).
const char* KnownTrigger(const std::string& id) {
    static const std::map<std::string, const char*> triggers = {
        {"01_rockbottom_cine", "load"},         {"03_carcrash_cine_03_part01", "load"},
        {"04_thisistheplace_cine", "node"},     {"08_emptytomb_cine", "node"},
        {"10_revelations_cine", "load"},        {"14_expedtion_a_cine", "load"},
        {"15_almostthere_cine", "load"},        {"20_alive_cine", "load"},
        {"21_whispersinthedark_cine", "node"},  {"22a_theenemy_cine", "load"},
        {"22b_sofia_cine", "node"},             {"23_aconnection_cine", "node"},
        {"27b_trustissues_cine", "node"},       {"36_meettheremnant_cine", "node"},
        {"39_shotgun_cine", "node"},            {"40_tothetower_cine", "node"},
        {"40b_courtyardincursion_cine", "node"}, {"40c_remnantresistance_cine", "node"},
        {"40d_planofaction_cine", "node"},      {"41_retreat_cine", "node"},
        {"44_cathedralambush_cine", "load"},    {"46_thedeathlessones_cine", "load"},
        {"47c_revealingthepath_cine", "node"},
        {"47d_achoice_cine", "node"},           {"48a_locatingthesource_cine", "node"},
        {"50_anoldfriend_cine", "node"},        {"51_tehrace_cine", "load"},
        {"56_theconstanceofkonstantin_cine", "node"}, {"57_thereckoning_cine", "node"},
        {"60_aftermath_cine", "load"},
    };
    const auto it = triggers.find(Lower(id));
    return it != triggers.end() ? it->second : "";
}
// The pictures in the viewer's info panel (cutscenes.json "pictures": width, height in the panel's units, x, y added to
// the picture's place; textureWidth, textureHeight the texture made from the file, cropped to that shape from its
// middle; frame: files without alpha given cutscenes\frame.png's edges).
std::mutex g_pictureMutex;
PictureLayout g_pictureLayout;
void ReadPictureLayout(const nlohmann::json& doc) {
    PictureLayout layout;
    if (doc.contains("pictures") && doc["pictures"].is_object()) {
        const auto& p = doc["pictures"];
        try {
            layout.width = std::clamp(p.value("width", layout.width), 16, 2048);
            layout.height = std::clamp(p.value("height", layout.height), 16, 2048);
            layout.x = p.value("x", layout.x);
            layout.y = p.value("y", layout.y);
            // (BC3 blocks: a multiple of 4)
            layout.textureWidth = std::clamp(p.value("textureWidth", layout.textureWidth), 16, 4096) / 4 * 4;
            layout.textureHeight = std::clamp(p.value("textureHeight", layout.textureHeight), 16, 4096) / 4 * 4;
            layout.frame = p.value("frame", layout.frame);
        } catch (...) {
        }
    }
    std::lock_guard lock(g_pictureMutex);
    g_pictureLayout = layout;
}

void ReadLayerList(const nlohmann::json& item, const std::string& id, std::vector<std::string>& layers,
                   const char* key = "layers") {
    const auto& known = std::strcmp(key, "layers") == 0             ? KnownLayers()
                        : std::strcmp(key, "layersOff") == 0        ? KnownLayersOff()
                        : std::strcmp(key, "layersOffAtStart") == 0 ? KnownLayersOffAtStart()
                                                                    : KnownUnits();
    layers.clear();
    if (!item.contains(key) || !item[key].is_array()) {
        if (auto it = known.find(Lower(id)); it != known.end()) layers = it->second;
        return;
    }
    for (const auto& name : item[key])
        if (name.is_string() && !name.get<std::string>().empty()) layers.push_back(name.get<std::string>());
}

void WriteCatalog(const std::vector<Entry>& entries) {
    nlohmann::json out = nlohmann::json::object();
    out["about"] = "The cutscene viewer's list. Edit title, category (Story, Baba Yaga, Blood Ties, Cold Darkness), "
                   "description, titles and descriptions (the title and description in the game's other text "
                   "languages, by its language code: fr, de, it, es-mx, es, ja, pt, pl, ru, nl, ko, cht, chs, ar, en; "
                   "Arabic in its reading order; a language without one shows title and description; titles are "
                   "written for the listed cutscenes while their title is the one they translate), image (a file in cutscenes\\images, JPEG or PNG: a screenshot of any size, cropped to "
                   "2:1 from its middle and given the edges of the game's own pictures; a PNG with alpha of its own "
                   "is used as it is; of <name>.jpg, .jpeg and .png the newest), hidden, "
                   "hide (objects whose instances are hidden while the viewer plays it), remove (objects whose instances are "
                   "removed from the level once it is loaded: what its start has running that the story has stopped by "
                   "then), layers (stream layers of its "
                   "unit turned on before it, as the log and cutscenes\\layers\\<unit>.txt name them), layersOff (stream "
                   "layers of its unit turned off before it, after those), layersOffAtStart (stream layers turned off "
                   "as the game starts it: what the story switches off between its node and its first frame; in the "
                   "three, <unit>:<layer> names a layer of another loaded unit), units (other units streamed in before "
                   "it, as the story has them loaded), "
                   "then (the "
                   "cutscenes the story plays right after it: the viewer plays them with it, and does not list them on "
                   "their own), storyEvent (\"<node>:<output>\": the event of another instance's graph that the story's way to "
                   "it starts from, a node of its graph relaying that graph and the output: the viewer sends it as that "
                   "graph would, and the scene comes the story's way; the level's delays still running in the graphs it reaches are "
                   "run out before it, and a delay right after its own relay passes at once; \"\" for none), hideNpcs (the level's people that "
                   "are not in the cutscene hidden while it plays), "
                   "keepLevelMusic (true: the music the level plays before the cutscene goes on; otherwise it is faded "
                   "out before the cutscene starts, and the cutscene's own music plays) and start (the Chapter Select "
                   "entry whose story plays it: unit and runnable, as in chapter_select.txt; via: a cutscene before it "
                   "whose node the viewer fires when the start's level has none for it); unit and kind are written by "
                   "the framework. pictures: their size in the viewer's info panel (width, height: 512x256 as the "
                   "game's own) and place (x, y: moved by that much), the texture made of each (textureWidth, "
                   "textureHeight: 4x that, shown at width x height) and frame (false: screenshots shown as they are, "
                   "without the edges of cutscenes\\frame.png).";
    {
        std::lock_guard lock(g_pictureMutex);
        out["pictures"] = {{"width", g_pictureLayout.width}, {"height", g_pictureLayout.height},
                           {"x", g_pictureLayout.x}, {"y", g_pictureLayout.y},
                           {"textureWidth", g_pictureLayout.textureWidth},
                           {"textureHeight", g_pictureLayout.textureHeight}, {"frame", g_pictureLayout.frame}};
    }
    out["cutscenes"] = nlohmann::json::array();
    for (const Entry& e : entries) {
        nlohmann::json item;
        item["id"] = e.id;
        item["title"] = e.title;
        item["category"] = e.category;
        item["description"] = e.description;
        if (!e.titles.empty()) item["titles"] = e.titles;
        if (!e.descriptions.empty()) item["descriptions"] = e.descriptions;
        item["image"] = e.image;
        item["hidden"] = e.hidden;
        if (!e.hide.empty()) item["hide"] = e.hide;
        if (!e.remove.empty()) item["remove"] = e.remove;
        if (!e.layers.empty()) item["layers"] = e.layers;
        if (!e.layersOff.empty()) item["layersOff"] = e.layersOff;
        if (!e.layersOffAtStart.empty()) item["layersOffAtStart"] = e.layersOffAtStart;
        if (!e.units.empty()) item["units"] = e.units;
        if (!e.then.empty()) item["then"] = e.then;
        if (!e.storyEvent.empty() || KnownStoryEvents().count(Lower(e.id))) item["storyEvent"] = e.storyEvent;
        if (e.hideNpcs || KnownHideNpcs(e.id)) item["hideNpcs"] = e.hideNpcs;
        if (e.keepLevelMusic) item["keepLevelMusic"] = true;
        item["kind"] = e.video ? "video" : "realtime";
        if (!e.unit.empty()) item["unit"] = e.unit;
        if (!e.startUnit.empty()) {
            item["start"] = {{"unit", e.startUnit}, {"runnable", e.startRunnable}};
            if (!e.trigger.empty()) item["start"]["trigger"] = e.trigger;
            if (!e.via.empty()) item["start"]["via"] = e.via;
        }
        out["cutscenes"].push_back(std::move(item));
    }
    std::ofstream file(CatalogPath(), std::ios::binary | std::ios::trunc);
    file << out.dump(2);
}

// (development) The game's Scaleform movies (dtp subtype 50 resources: "GFX" after a 4-byte size) packed into
// ROTTRFramework/dump/ui_movies.pack once, for the native Extra screen's work: "UIMP", count, then per movie the .drm's
// name hash, the resource index and ID, its size and bytes.
void DumpUiMovies(ArchiveSet& set, const fs::path& root) {
    const fs::path out = root / L"ROTTRFramework" / L"dump" / L"ui_movies.pack";
    std::error_code ec;
    if (fs::exists(out, ec)) return;
    fs::create_directories(out.parent_path(), ec);
    std::vector<std::uint8_t> pack = {'U', 'I', 'M', 'P', 0, 0, 0, 0};
    std::uint32_t count = 0;
    std::set<std::tuple<int, int, std::uint32_t, std::uint32_t>> seen;
    const auto started = GetTickCount64();
    for (std::size_t a = 0; a < set.archives.size(); ++a) {
        if (set.archives[a].language) continue;
        for (const auto& f : set.archives[a].files) {
            if (f.size > (8u << 20)) continue;
            const GameArchive::Location where{f.archiveId, f.part, f.offset, f.compressedSize ? f.compressedSize : f.size,
                                              int(a)};
            std::vector<std::uint8_t> bytes;
            GameArchive::Drm drm;
            if (!set.ReadBlob(where, bytes, f.size) || !drm.Parse(bytes)) continue;
            for (std::size_t i = 0; i < drm.ids.size(); ++i) {
                if (drm.ids[i].type != GameArchive::kTypeDtp || ((drm.ids[i].subTypeAndRefSize >> 1) & 0x7F) != 50) continue;
                const auto& l = drm.locations[i];
                if (!seen.insert({l.archiveId, l.part, l.offset, l.decompressionOffset}).second) continue;
                std::vector<std::uint8_t> data;
                if (!set.ReadBlob(drm.LocationOf(i), data, drm.TotalSize(i))) continue;
                GameArchive::Append<std::uint32_t>(pack, f.nameHash);
                GameArchive::Append<std::uint32_t>(pack, std::uint32_t(i));
                GameArchive::Append<std::int32_t>(pack, drm.ids[i].id);
                GameArchive::Append<std::uint32_t>(pack, std::uint32_t(data.size()));
                pack.insert(pack.end(), data.begin(), data.end());
                ++count;
            }
        }
    }
    GameArchive::Put<std::uint32_t>(pack, 4, count);
    std::ofstream file(out, std::ios::binary);
    file.write(reinterpret_cast<const char*>(pack.data()), std::streamsize(pack.size()));
    Log::Writef("Cutscenes: %u UI movies written to dump\\ui_movies.pack (%zu KB, %llu ms)", count, pack.size() / 1024,
                static_cast<unsigned long long>(GetTickCount64() - started));
}

void ScanThread() {
    try {
        ArchiveSet set;
        set.specialisationMask = GameData::kEnglishMask;
        std::string error;
        const fs::path root = GameRoot();
        if (!set.Open(root, error)) {
            std::lock_guard lock(g_catalogMutex);
            g_scanStatus = "the game's archives could not be read: " + error;
            g_scanState = 3;
            Log::Writef("Cutscenes: %s", g_scanStatus.c_str());
            return;
        }
        // (the object names: the game's own zone table, once it has loaded it)
        std::map<std::uint32_t, std::string> zones;
        for (int wait = 0; wait < 600 && !ZoneList::Ready(); ++wait) Sleep(100);
        for (std::uint32_t id = 1, max = ZoneList::MaxId(); id <= max; ++id)
            if (const char* name = ZoneList::Name(id)) zones[id] = name;
        const auto started = GetTickCount64();
        std::vector<Entry> found;
        for (const auto& name : FindNames(set, zones)) {
            Entry e;
            e.id = name;
            e.unit = UnitOf(set, name);
            e.archive = ArchiveOf(set, MulPath(name));
            found.push_back(std::move(e));
        }
        std::error_code ec;
        for (const auto& file : fs::directory_iterator(root / L"movies" / L"pc", ec)) {
            if (Lower(file.path().extension().string()) != ".bik") continue;
            Entry e;
            e.id = file.path().stem().string();
            e.video = true;
            found.push_back(std::move(e));
        }

        // the user's file: what it says wins; new cutscenes added
        nlohmann::json doc = nlohmann::json::object();
        {
            std::ifstream in(CatalogPath());
            if (in) {
                try {
                    doc = nlohmann::json::parse(in);
                } catch (...) {
                    Log::Write("Cutscenes: cutscenes.json could not be read (kept as cutscenes.json.bad; a new one written)");
                    fs::rename(CatalogPath(), fs::path(CatalogPath()).concat(L".bad"), ec);
                    doc = nlohmann::json::object();
                }
            }
        }
        ReadPictureLayout(doc);
        std::map<std::string, nlohmann::json> saved;
        if (doc.contains("cutscenes") && doc["cutscenes"].is_array())
            for (const auto& item : doc["cutscenes"])
                if (item.is_object() && item.contains("id") && item["id"].is_string())
                    saved[Lower(item["id"].get<std::string>())] = item;
        int added = 0;
        for (Entry& e : found) {
            const auto it = saved.find(Lower(e.id));
            const nlohmann::json item = it != saved.end() ? it->second : nlohmann::json::object();
            if (it == saved.end()) ++added;
            e.title = FileTitle(item, e.id, DefaultTitle(e.id));
            e.category = item.value("category", e.video ? std::string("Story") : CategoryOf(e.id, e.archive));
            e.description = item.value("description", std::string());
            ReadTranslations(item, e);
            e.image = item.value("image", e.id + ".jpg");
            e.hidden = item.value("hidden", false);
            ReadHideList(item, e.hide);
            ReadRemoveList(item, e.id, e.remove);
            ReadLayerList(item, e.id, e.layers);
            ReadLayerList(item, e.id, e.layersOff, "layersOff");
            ReadLayerList(item, e.id, e.layersOffAtStart, "layersOffAtStart");
            ReadLayerList(item, e.id, e.units, "units");
            ReadThenList(item, e.id, e.then);
            ReadFlags(item, e);
            ReadStoryEvent(item, e);
            e.trigger = KnownTrigger(e.id);
            if (item.contains("start") && item["start"].is_object()) {
                e.startUnit = item["start"].value("unit", std::string());
                e.startRunnable = item["start"].value("runnable", std::string());
                e.trigger = item["start"].value("trigger", e.trigger);
                e.via = item["start"].value("via", std::string());
                e.startFromFile = !e.startUnit.empty();
            }
        }
        std::stable_sort(found.begin(), found.end(), [](const Entry& a, const Entry& b) {
            if (a.category != b.category) return a.category < b.category;
            return SortKey(a) < SortKey(b);
        });
        fs::create_directories(CatalogPath().parent_path() / L"images", ec);
        WriteCatalog(found);
        int realtime = 0;
        for (const Entry& e : found) realtime += !e.video;
        char status[160];
        std::snprintf(status, sizeof(status), "%d cutscenes and %d videos (%d new in cutscenes.json), %llu ms", realtime,
                      int(found.size()) - realtime, added, static_cast<unsigned long long>(GetTickCount64() - started));
        {
            std::lock_guard lock(g_catalogMutex);
            g_catalog = std::move(found);
            g_scanStatus = status;
        }
        g_scanState = 2;
        Log::Writef("Cutscenes: %s", status);
        DumpUiMovies(set, root);
    } catch (const std::exception& ex) {
        std::lock_guard lock(g_catalogMutex);
        g_scanStatus = std::string("the scan failed: ") + ex.what();
        g_scanState = 3;
        Log::Writef("Cutscenes: %s", g_scanStatus.c_str());
    }
}

// ---- playing (game thread) ----
// LoadingUnit: the load asked for; Settling: the unit is there, waiting for the game's loading to end (0.9.341 started
// ours during the load: the loading screen never went); Watching: the load is over, the unit may play its own cutscene
// (cine units start theirs themselves); Playing: ours; LevelPlaying: the one the unit started.
enum class Phase { Idle, LoadingUnit, Settling, Watching, Playing, LevelPlaying };
struct Request {
    bool pending = false;
    bool stop = false;
    bool load = false;   // load its unit first
    std::string id, unit;
    std::uint16_t chapterUnit = 0;   // through a Chapter Select entry (its unit ID and runnable), even when loaded
    std::string runnable;
    bool storyPlays = false;         // (a cine unit loaded by name for the viewer: its own scripts play it, or its node)
    std::string via;                 // (Entry::via)
};
std::mutex g_requestMutex;
Request g_request;
std::atomic<Phase> g_phase{Phase::Idle};
std::string g_playId, g_playUnit;
ULONGLONG g_phaseSince = 0, g_lastFrameLog = 0, g_loadAskedAt = 0;
void* g_impl = nullptr;   // the NtCinematic: a pointer to the impl
bool g_seenPlaying = false;
bool g_viaChapter = false;            // loaded through a Chapter Select entry: the game's story plays the cutscene
std::string g_lastSeenCine;           // the cutscene the game played last (its handler's name)
std::atomic<bool> g_returnToMenu{true};
std::atomic<bool> g_viewerMode{false};   // started by the Cutscene Viewer: no direct-start fallback, back to the menu
std::atomic<bool> g_abortRequested{false};
std::atomic<float> g_secondsLeft{-1.0f};
std::atomic<bool> g_viewerFailed{false};
std::atomic<bool> g_nodeHold{false};     // the viewer is not ready for the node yet (its outfit swap)
ULONGLONG g_quietSince = 0;              // Watching: no cutscene playing since
int g_nodeFires = 0;                     // the cutscene's node fired (Watching)
std::string g_playStoryEvent;            // Entry::storyEvent of the one played
ULONGLONG g_storyEventAt = 0;            // when it was sent (0: not)
ULONGLONG g_watchSince = 0;              // when Watching began after the load
bool g_storyEventWaitLogged = false;
// In the story a story event comes long after the level began (Retreat's: the mini-boss fight's end), when the level's
// own delays from its start have run out. 0.9.382 sent Retreat's 4.5 s after the load: the explosions' graph stopped its
// timer (node 14, input 2), then its 5 s AGTimeDelayNode from the player's spawn (node 10 "510 Player Spawned" output 2
// -> node 11 -> 4 -> 3 -> 15 -> the stopwatch's start) started it again 0.6 s later, and the shakes went on. 0.9.383 to
// 0.9.384 waited 8 s after the load for them (5.1 s of Retreat's 14.3 s of black). Now the delays running in the graphs
// the event reaches are run out at once (RunOutStoryDelays: their output sent as their timer's end would), 1.5 s after
// the load at the earliest (the player's spawn is signalled with the load's end), then 0.5 s for what they started;
// again while more are running, 3 rounds at most; then the event.
constexpr ULONGLONG kStoryEventAfterLoad = 1500, kStoryRoundMs = 500;
constexpr int kStoryRounds = 3;
int g_storyRounds = 0;                   // the rounds of the level's delays run out
ULONGLONG g_storyRoundAt = 0;            // the last one

// ---- the black after the load (the viewer's plays): where its time goes, logged at the cutscene's first frame ----
// Each tick from the game's load's end to the cutscene's first frame is put down to what the viewer waited for then.
enum class Wait { None, Settling, OtherCine, Loading, Quiet, Outfit, NoNode, Units, Layers, Placements, StoryEventWait,
                  StoryWay, Stream, Count };
constexpr const char* kWaitNames[] = {"other", "the 0.5 s after the load", "another cutscene playing (skipped)",
                                      "the game loading again", "the quiet wait before its node", "Lara's outfit swap",
                                      "no node of it yet", "units streaming in", "its stream layers",
                                      "the level's placements being born",
                                      "the level's delays run out before its story event",
                                      "the story's way after its event", "its stream after its node fired"};
static_assert(sizeof(kWaitNames) / sizeof(kWaitNames[0]) == std::size_t(Wait::Count));
ULONGLONG g_waitMs[std::size_t(Wait::Count)] = {};
Wait g_waitReason = Wait::None;
ULONGLONG g_waitTick = 0;   // (0: not measuring)
ULONGLONG g_loadEndAt = 0;
bool g_blackLogged = false;
bool g_nodeMissingLogged = false;
bool g_nodeBusyLogged = false;
ULONGLONG g_nodeReportAt = 0;            // the fired node's stream, logged every 2 s until it plays
int g_nodeReports = 0, g_actorReports = 0;
float g_stallFrame = -1.0f;              // LevelPlaying: the frame and since when it stands
ULONGLONG g_stallSince = 0;
bool g_stallLogged = false;
bool g_lengthLogged = false;
ULONGLONG g_birthWaitSince = 0;
bool g_birthLogged = false;
std::string g_playVia;                   // (Entry::via) its node fired when the level has none for ours
std::string g_fireId;                    // the cutscene whose node was fired (ours, or the via)
bool g_viaUsed = false;
std::string g_skipName;                  // viewer: the other cutscene the level plays, skipped (tries, next try)
int g_skipTries = 0;
ULONGLONG g_skipAt = 0;
bool g_reloadLogged = false;             // Watching: the game loads again (the story goes on in another unit)
std::string g_cineUnit;                  // the unit whose cutscene list has ours (Entry::unit)
std::vector<int> g_layerUnits;           // Watching: the units whose layers of ours were looked for (once each)
int g_layersOn = 0;                      // the layers of ours turned on, and when the last were
ULONGLONG g_layersAt = 0;
ULONGLONG g_noNodeSince = 0;             // Watching (viewer): no node of ours (nor of its way there) since
bool g_directStart = false;              // Playing: started directly by the viewer (no node of it in the level)
bool g_directMissLogged = false;
bool g_nodeKnown = false;                // its trigger (or its way there's) is known as "node": no quiet wait for
                                         // the level to start it itself
std::vector<std::string> g_chain;        // the cutscenes the story plays right after ours (Entry::then)
std::size_t g_chainStep = 0;             // how many of them were reached (g_playId is the current one)
std::atomic<ULONGLONG> g_chainGapSince{0};   // Watching: between two of the chain, since
bool g_playHideNpcs = false;             // Entry::hideNpcs / keepLevelMusic of the one playing
bool g_playKeepMusic = false;
constexpr float kCineFps = 30.0f;        // (the handler's frames: 300 every 10 s in the 0.9.341 logs)
constexpr std::size_t kHandlerStreamOffset = 0x08, kStreamLengthOffset = 0x388, kStreamStateOffset = 0x128;
std::atomic<int> g_frame{-1};
char g_playStatus[256] = "idle";

void SetStatus(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    vsnprintf(g_playStatus, sizeof(g_playStatus), fmt, args);
    va_end(args);
    Log::Writef("Cutscenes: %s", g_playStatus);
}

// The loaded unit's stream ID by name (0: not loaded). SEH only.
int LoadedUnitId(const char* name) noexcept {
    __try {
        auto* tracker = reinterpret_cast<std::uint8_t*>(GameBuild::Base() + kRvaStreamTracker);
        for (int i = 0; i < kUnitCount; ++i) {
            std::uint8_t* unit = tracker + std::size_t(i) * kUnitStride;
            const int state = unit[4] & 0x7F;
            if (state == 0 || state == 4) continue;
            if (_stricmp(reinterpret_cast<const char*>(unit + kUnitNameOffset), name) == 0)
                return *reinterpret_cast<const int*>(unit);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    return 0;
}

// The cutscene's index in the unit's list (-1: not listed; -2: the unit's data is not there yet). SEH only.
int CineIndex(int unitId, const char* name) noexcept {
    __try {
        auto* level = static_cast<std::uint8_t*>(g_getLevel(unitId));
        if (!level) return -2;
        auto* unitData = *reinterpret_cast<std::uint8_t**>(level + kLevelUnitDataOffset);
        if (!unitData) return -2;
        const std::uint32_t count = *reinterpret_cast<std::uint32_t*>(unitData + kUnitDataCineCountOffset);
        auto** list = *reinterpret_cast<const char***>(unitData + kUnitDataCineListOffset);
        for (std::uint32_t i = 0; i < count && list; ++i)
            if (list[i] && _stricmp(list[i], name) == 0) return int(i);
        return -1;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -2;
    }
}

// ---- the cutscene's stream layers ----
// A unit's stream layers (UnitData +0x830: entries of 0x30 bytes, the name first; +0x838 their count. Retail code reads
// them so at 0x40CAD2, TR2 GetStreamLayerName 0x42FBF0; the game data's UnitData has them there) hold,
// among the rest, each cutscene's props and actors: sh_soviet_hub's Cine_Props_Scene_23_A_Connection (index 226) holds A
// Connection's. The story turns the layer on before the scene (an action graph's UnitStreamLayerNode); the viewer fired
// the node without it in 0.9.360, and its 18 actors were MISSING (the stream stood in state 3). The viewer turns it on as
// that node does: StreamLayerManager::ActivateStreamLayer(manager, unit ID, layer) (retail 0x3F51F0, TR2 0x41FA30:
// SetStreamLayerSafely(GenerateHashKeyForUnitObject(unit), layer, 2); its other caller NsUnit::ActivateStreamLayer, retail
// 0x1F57D0, takes the manager from StreamLayerManager::Get, retail 0x401FB0).
constexpr std::uintptr_t kRvaStreamLayerManagerGet = 0x00401FB0ull, kRvaActivateStreamLayer = 0x003F51F0ull;
constexpr std::uint8_t kStreamLayerManagerGetSig[] = {0x48, 0x8B, 0x05, 0xC1, 0xEA, 0x92, 0x02, 0xC3};
constexpr std::uint8_t kActivateStreamLayerSig[] = {0x48, 0x89, 0x5C, 0x24, 0x08, 0x57, 0x48, 0x83, 0xEC, 0x20, 0x48,
                                                    0x8B, 0xF9, 0x41, 0x8B, 0xD8, 0x48, 0x8D, 0x4C, 0x24, 0x48, 0xE8};
constexpr std::size_t kUnitDataLayerListOffset = 0x830, kUnitDataLayerCountOffset = 0x838, kLayerEntrySize = 0x30;
constexpr int kMaxLayers = 1024, kLayerNameSize = 96;
bool g_layersReady = false;
char g_layerNames[kMaxLayers][kLayerNameSize];   // (game thread)

// The unit's stream layer names, by index (-1: its data is not there). SEH only.
int UnitLayerNames(int unitId) noexcept {
    __try {
        auto* level = static_cast<std::uint8_t*>(g_getLevel(unitId));
        auto* unitData = level ? *reinterpret_cast<std::uint8_t**>(level + kLevelUnitDataOffset) : nullptr;
        if (!unitData) return -1;
        const std::uint32_t count = *reinterpret_cast<std::uint32_t*>(unitData + kUnitDataLayerCountOffset);
        auto* list = *reinterpret_cast<std::uint8_t**>(unitData + kUnitDataLayerListOffset);
        if (!list || count > 4096) return -1;
        const int n = int(std::min<std::uint32_t>(count, kMaxLayers));
        for (int i = 0; i < n; ++i) {
            const char* name = *reinterpret_cast<const char**>(list + std::size_t(i) * kLayerEntrySize);
            int k = 0;
            for (; name && k + 1 < kLayerNameSize && name[k]; ++k) g_layerNames[i][k] = name[k];
            g_layerNames[i][k] = 0;
        }
        return n;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
}

std::atomic<bool> g_viewerLayerCall{false};   // (ours: the switch log names it)
bool ActivateLayer(int unitId, int layer) noexcept {
    __try {
        void* manager = reinterpret_cast<void*(__fastcall*)()>(GameBuild::Base() + kRvaStreamLayerManagerGet)();
        if (!manager) return false;
        reinterpret_cast<void(__fastcall*)(void*, int, int)>(GameBuild::Base() + kRvaActivateStreamLayer)(manager, unitId, layer);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// StreamLayerManager::DeactivateStreamLayer(manager, unit ID, layer) (retail 0x3FE970, TR2 0x429810): as
// ActivateStreamLayer, with action 3 (deactivate). Its bytes up to that action are checked.
constexpr std::uintptr_t kRvaDeactivateStreamLayer = 0x003FE970ull;
constexpr std::uint8_t kDeactivateStreamLayerSig[] = {0x48, 0x89, 0x5C, 0x24, 0x08, 0x57, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x8B,
                                                      0xF9, 0x41, 0x8B, 0xD8, 0x48, 0x8D, 0x4C, 0x24, 0x48, 0xE8};
constexpr std::uint8_t kDeactivateStreamLayerActionSig[] = {0x48, 0x8B, 0x54, 0x24, 0x48, 0x41, 0xB1, 0x03};
bool g_layersOffReady = false;
bool DeactivateLayer(int unitId, int layer) noexcept {
    if (!g_layersOffReady) return false;
    __try {
        void* manager = reinterpret_cast<void*(__fastcall*)()>(GameBuild::Base() + kRvaStreamLayerManagerGet)();
        if (!manager) return false;
        reinterpret_cast<void(__fastcall*)(void*, int, int)>(GameBuild::Base() + kRvaDeactivateStreamLayer)(manager, unitId, layer);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// The layers the viewer turned on for this play (checked again before the node is fired).
struct TurnedOn {
    int unit, layer, tries;
};
std::vector<TurnedOn> g_turnedOn;
bool TurnOnLayer(int unitId, int layer) {
    g_viewerLayerCall = true;
    const bool ok = ActivateLayer(unitId, layer);
    g_viewerLayerCall = false;
    if (ok && std::none_of(g_turnedOn.begin(), g_turnedOn.end(),
                           [&](const TurnedOn& t) { return t.unit == unitId && t.layer == layer; }))
        g_turnedOn.push_back({unitId, layer, 1});
    return ok;
}

// The layers the viewer turned off for this play (checked again before the node is fired, as g_turnedOn).
std::vector<TurnedOn> g_turnedOff;
bool TurnOffLayer(int unitId, int layer) {
    g_viewerLayerCall = true;
    const bool ok = DeactivateLayer(unitId, layer);
    g_viewerLayerCall = false;
    if (ok && std::none_of(g_turnedOff.begin(), g_turnedOff.end(),
                           [&](const TurnedOn& t) { return t.unit == unitId && t.layer == layer; }))
        g_turnedOff.push_back({unitId, layer, 1});
    return ok;
}

std::string AlnumLower(const std::string& s) {
    std::string out;
    for (const char c : s) {
        const char l = char(std::tolower(static_cast<unsigned char>(c)));
        if ((l >= 'a' && l <= 'z') || (l >= '0' && l <= '9')) out.push_back(l);
    }
    return out;
}

// How well a stream layer's name fits the cutscene (0: not at all; only layers named "cine..."). The cutscene's name
// ("27_jacob_cine") gives its scene key ("27": the leading digits and the letters right after them) and its title
// ("jacob"); the layers name both ("Cine_Props_Scene_27A_Jacob"), not always as the cutscene does (28_atlas_cine:
// Cine_Props_Scene_28_The_Atlas; 31a_aloneagain_cine: Cine_Props_Scene_31_Alone_Again). The whole name in the layer's
// counts 4, the title 2, the key as a word of the layer 2 (or 1: "27a" for "27", "31" for "31a"): for Jacob,
// Cine_Props_Scene_27A_Jacob fits 3, the next scene's Cine_Props_Scene_27_2_Wait_Here 2, cine_jacobFX 2.
int LayerFit(const std::string& layer, const std::string& id) {
    std::string lower = Lower(layer);
    if (lower.rfind("cine", 0) != 0) return 0;
    std::string stem = AlnumLower(id);
    if (stem.size() > 4 && stem.compare(stem.size() - 4, 4, "cine") == 0) stem.resize(stem.size() - 4);
    std::string digits, key;
    const std::string bareId = Lower(id);
    std::size_t at = 0;
    while (at < bareId.size() && std::isdigit(static_cast<unsigned char>(bareId[at]))) digits.push_back(bareId[at++]);
    key = digits;
    while (!digits.empty() && at < bareId.size() && std::isalpha(static_cast<unsigned char>(bareId[at]))) key.push_back(bareId[at++]);
    const std::string title = stem.substr(std::min(stem.size(), key.size()));
    const std::string flat = AlnumLower(layer);
    int fit = 0;
    if (!stem.empty() && flat.find(stem) != std::string::npos) fit += 4;
    if (title.size() >= 3 && flat.find(title) != std::string::npos) {
        fit += 2;
    } else if (title.size() >= 6 && flat.size() > 4) {
        // a shortened title: the layer's name after "cine", every letter in the title's order, at least 4/5 of it (Plan
        // of Action: br_010_outer_city's cine_planaction, "planaction" in "planofaction")
        const std::string rest = flat.substr(4);
        std::size_t k = 0;
        for (const char c : title)
            if (k < rest.size() && rest[k] == c) ++k;
        if (k == rest.size() && rest.size() >= 6 && rest.size() * 5 >= title.size() * 4) fit += 2;
    }
    if (!key.empty()) {
        int best = 0;
        std::string token;
        for (std::size_t i = 0; i <= lower.size(); ++i) {
            const char c = i < lower.size() ? lower[i] : '_';
            if (std::isalnum(static_cast<unsigned char>(c))) {
                token.push_back(c);
                continue;
            }
            if (token == key) best = 2;
            else if (key != digits && token == digits) best = std::max(best, 1);
            else if (key == digits && token.size() > digits.size() && token.rfind(digits, 0) == 0 &&
                     std::isalpha(static_cast<unsigned char>(token[digits.size()])))
                best = std::max(best, 1);
            token.clear();
        }
        fit += best;
    }
    return fit;
}

// Turns on the unit's stream layer(s) of the cutscene: those that fit it best (3 or more; 2 when only one layer does).
// The count turned on (-1: the unit's data is not there yet); the choice (or the unit's cutscene layers, when none fits)
// logged.
int ActivateCineLayers(int unitId, const std::string& id) {
    if (!g_layersReady || unitId <= 0) return 0;
    const int count = UnitLayerNames(unitId);
    if (count < 0) return -1;
    if (count == 0) {
        Log::Writef("Cutscenes: %s: unit %d has no stream layers", id.c_str(), unitId);
        return 0;
    }
    std::vector<int> fit(std::size_t(count), 0);
    int top = 0;
    for (int i = 0; i < count; ++i) top = std::max(top, fit[std::size_t(i)] = LayerFit(g_layerNames[i], id));
    // (of two that fit as well, a variant named after the other is left: Cine_Props_01_Rock_Bottom_HangingLight_On)
    for (int i = 0; i < count && top > 0; ++i)
        for (int j = 0; j < count && fit[std::size_t(i)] == top; ++j)
            if (j != i && fit[std::size_t(j)] == top && std::strlen(g_layerNames[j]) < std::strlen(g_layerNames[i]) &&
                _strnicmp(g_layerNames[i], g_layerNames[j], std::strlen(g_layerNames[j])) == 0)
                fit[std::size_t(i)] = top - 1;
    int tops = 0;
    for (int i = 0; i < count; ++i) tops += top > 0 && fit[std::size_t(i)] == top;
    std::string text;
    int on = 0;
    if (top >= 3 || (top == 2 && tops == 1)) {
        for (int i = 0; i < count; ++i) {
            if (fit[std::size_t(i)] != top) continue;
            const bool ok = TurnOnLayer(unitId, i);
            on += ok;
            char item[160];
            snprintf(item, sizeof(item), "%s%s (%d)%s", text.empty() ? "" : ", ", g_layerNames[i], i, ok ? "" : " FAILED");
            text += item;
        }
        Log::Writef("Cutscenes: %s: its stream layer turned on in unit %d, as the story does before it: %s", id.c_str(),
                    unitId, text.c_str());
        return on;
    }
    int listed = 0;
    for (int i = 0; i < count && listed < 24; ++i) {
        if (Lower(g_layerNames[i]).rfind("cine", 0) != 0) continue;
        text += (listed++ ? ", " : "") + std::string(g_layerNames[i]);
    }
    Log::Writef("Cutscenes: %s: no stream layer of unit %d is its own (%d layers; those named cine...: %s)", id.c_str(), unitId,
                count, listed ? text.c_str() : "none");
    return 0;
}

bool AnyCinematicPlaying() noexcept {
    __try {
        void* handler = *reinterpret_cast<void**>(GameBuild::Base() + kRvaCurrentCineHandler);
        return handler && g_handlerIsPlaying(handler, true);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// The current cutscene: playing, its name and frame. SEH only.
bool CurrentCine(char* name, std::size_t capacity, float* frame) noexcept {
    name[0] = 0;
    __try {
        auto* handler = *reinterpret_cast<std::uint8_t**>(GameBuild::Base() + kRvaCurrentCineHandler);
        if (!handler || !g_handlerIsPlaying(handler, true)) return false;
        const char* n = *reinterpret_cast<const char**>(handler + kHandlerNameOffset);
        if (n) {
            std::size_t i = 0;
            for (; i + 1 < capacity && n[i]; ++i) name[i] = n[i];
            name[i] = 0;
        }
        if (frame) *frame = *reinterpret_cast<float*>(handler + kHandlerFrameOffset);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        name[0] = 0;
        return false;
    }
}

// The current cutscene's length in seconds (its MultiplexStream, handler +8: +0x388 once past state 2; GetMediaLength
// retail 0x3A5010). -1: not known.
// The current cutscene's frame and frame count (CinematicHandler +0x40 / +0x44, as NtCinematic::GetPercentComplete,
// retail 0x1FA1B0, divides them). The stream's media length (+0x388) ran past the cutscene's end: no fade-out (0.9.352).
bool CurrentCineFrames(int& frame, int& total) noexcept {
    __try {
        auto* handler = *reinterpret_cast<std::uint8_t**>(GameBuild::Base() + kRvaCurrentCineHandler);
        if (!handler) return false;
        frame = *reinterpret_cast<int*>(handler + 0x40);
        total = *reinterpret_cast<int*>(handler + 0x44);
        return total > 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

float CurrentCineLength() noexcept {
    __try {
        auto* handler = *reinterpret_cast<std::uint8_t**>(GameBuild::Base() + kRvaCurrentCineHandler);
        auto* stream = handler ? *reinterpret_cast<std::uint8_t**>(handler + kHandlerStreamOffset) : nullptr;
        if (!stream || *reinterpret_cast<int*>(stream + kStreamStateOffset) <= 2) return -1.0f;
        return *reinterpret_cast<float*>(stream + kStreamLengthOffset);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1.0f;
    }
}

bool ReturnToMainMenu() noexcept {
    __try {
        using GetFn = void*(__fastcall*)();
        using ReturnFn = void(__fastcall*)(void*, bool);
        void* netManager = reinterpret_cast<GetFn>(GameBuild::Base() + kRvaNetManagerGet)();
        if (!netManager) return false;
        reinterpret_cast<ReturnFn>(GameBuild::Base() + kRvaReturnToMainMenu)(netManager, true);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool g_statesReady = false;
void ReadGameStates(int& game, int& net) noexcept {
    game = net = -1;
    if (!g_statesReady) return;
    __try {
        void* netManager = reinterpret_cast<void*(__fastcall*)()>(GameBuild::Base() + kRvaNetManagerGet)();
        if (!netManager) return;
        game = reinterpret_cast<int(__fastcall*)(void*)>(GameBuild::Base() + kRvaGetCurrGameState)(netManager);
        net = reinterpret_cast<int(__fastcall*)(void*)>(GameBuild::Base() + kRvaGetCurrNetState)(netManager);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        game = net = -1;
    }
}

// (the handler's name can be a path or carry a different case)
std::string BareCine(const char* current) {
    std::string c = Lower(current);
    if (const auto slash = c.find_last_of("\\/"); slash != std::string::npos) c = c.substr(slash + 1);
    if (const auto dot = c.rfind('.'); dot != std::string::npos) c.resize(dot);
    return c;
}
bool SameCine(const char* current, const std::string& id) { return BareCine(current) == Lower(id); }

bool StartCine(int index, int unitId) noexcept {
    __try {
        g_impl = g_cineGet(std::uint32_t(unitId), std::uint32_t(index));
        return g_impl && g_cinePlay(g_impl, true);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        g_impl = nullptr;
        return false;
    }
}

bool CineIsPlaying() noexcept {
    __try {
        return g_impl && g_cineIsPlaying(g_impl);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// Our stream's state (MultiplexStream: +0x128 state, 4 = ready; +0x3E3D play requested, +0x3E40 started, +0x3E3F
// done, +0x3DF8 its CinematicHandler). SEH only.
constexpr std::uintptr_t kRvaFindStream = 0x003D8850ull;   // EVENT_FindMultiplexStream(id)
bool StreamState(void* impl, int& id, int& state, int& requested, int& started, int& done, int& handler) noexcept {
    __try {
        id = impl ? *reinterpret_cast<int*>(static_cast<std::uint8_t*>(impl) + 4) : 0;
        if (!id) return false;
        using FindFn = std::uint8_t*(__fastcall*)(int);
        std::uint8_t* stream = reinterpret_cast<FindFn>(GameBuild::Base() + kRvaFindStream)(id);
        if (!stream) return false;
        state = *reinterpret_cast<int*>(stream + 0x128);
        requested = stream[0x3E3D];
        started = stream[0x3E40];
        done = stream[0x3E3F];
        handler = *reinterpret_cast<void**>(stream + 0x3DF8) != nullptr;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// Why a created cutscene waits in state 3: MultiplexStream::Update moves it to 4 (ready) only when its
// CinematicHandler's ReadyToPlay (retail 0x16BBF0) says so: the handler's +0x348 set and every actor (+0xA0 count,
// +0xA8 list, 0xF0 each, +0 its placement key) found as an Instance (GetCineActorInstance 0x1691D0) or at least queued
// for birth (PlacementManager::IsQueued 0x40C7F0). Written into text. SEH only.
constexpr std::uintptr_t kRvaReadyToPlay = 0x0016BBF0ull, kRvaGetCineActor = 0x001691D0ull, kRvaIsQueued = 0x0040C7F0ull;
// (retail ReadyToPlay 0x16BBF0, TR2 0x17F500) keys -1..-255: s_getCinematicInstanceCallback (retail 0x11042A8; the
// game's WORLD callback: -1 the player, the others the player's inventory items: -102 type 2, -108 type 0x48, ...);
// NULL = not ready. A special with Instance +0xC8 bit 5 (a player) is also checked for its busy byte and
// "delayed_death_pending"; at least one must pass. Then MOVIE_IsPlaying (retail 0x1EBE00) and the bytes 0x2D573F6 /
// 0x2D573F7 must be 0.
constexpr std::uintptr_t kRvaCineInstanceCallback = 0x011042A8ull, kRvaMovieIsPlaying = 0x001EBE00ull,
                         kRvaReadyBlockA = 0x02D573F6ull, kRvaReadyBlockB = 0x02D573F7ull;
// GetCineActorInstance (TR2 0x17CAA0) finds an actor under its unit's root instance: InstanceManager::FindByKey (retail
// 0x3DCCD0) of PlacementRef::GenerateHashKeyForUnitObject(unit) (retail 0x234110; the unit: handler +0x2C), the actor's
// key hashed onto that root's +0x60; without the root (the unit not loaded) every actor is missing. When the unit at
// +0x30 differs, MacroPermutationManager::GetReplacementCineActorInDerivedUnit may name another placement.
constexpr std::uintptr_t kRvaUnitObjectKey = 0x00234110ull, kRvaFindInstanceByKey = 0x003DCCD0ull;
constexpr std::uint8_t kUnitObjectKeySig[] = {0x85, 0xD2, 0x8B, 0xC2, 0x49, 0xB8, 0x25, 0x23, 0x22, 0x84};
constexpr std::uint8_t kFindInstanceByKeySig[] = {0x48, 0x3B, 0x0D, 0xA1, 0x4E, 0x10, 0x02, 0x4C, 0x8B, 0xC2};
void ActorReport(void* impl, char* out, std::size_t capacity) noexcept {
    out[0] = 0;
    __try {
        const int id = impl ? *reinterpret_cast<int*>(static_cast<std::uint8_t*>(impl) + 4) : 0;
        using FindFn = std::uint8_t*(__fastcall*)(int);
        std::uint8_t* stream = id > 0 ? reinterpret_cast<FindFn>(GameBuild::Base() + kRvaFindStream)(id) : nullptr;
        std::uint8_t* handler = stream ? *reinterpret_cast<std::uint8_t**>(stream + 0x3DF8) : nullptr;
        if (!handler) {
            snprintf(out, capacity, "no handler");
            return;
        }
        using ReadyFn = bool(__fastcall*)(void*, bool);
        using ActorFn = void*(__fastcall*)(void*, std::uint64_t, int, std::uint64_t*);
        using QueuedFn = bool(__fastcall*)(std::uint64_t, bool, bool, bool);
        using SpecialFn = void*(__fastcall*)(int, int);
        using MovieFn = bool(__fastcall*)();
        const std::uintptr_t base = GameBuild::Base();
        const bool ready = reinterpret_cast<ReadyFn>(base + kRvaReadyToPlay)(handler, true);
        const int count = *reinterpret_cast<int*>(handler + 0xA0);
        auto* actors = *reinterpret_cast<std::uint8_t**>(handler + 0xA8);
        auto special = *reinterpret_cast<SpecialFn*>(base + kRvaCineInstanceCallback);
        const int unit = *reinterpret_cast<int*>(handler + 0x2C), otherUnit = *reinterpret_cast<int*>(handler + 0x30);
        const char* unitName = ZoneList::Name(std::uint16_t(unit));
        const char* otherName = ZoneList::Name(std::uint16_t(otherUnit));
        int root = -1;   // its unit's root instance: 1 there, 0 not (the unit not loaded), -1 not checked
        if (GameBuild::VerifyBytes(kRvaUnitObjectKey, kUnitObjectKeySig, sizeof(kUnitObjectKeySig)) &&
            GameBuild::VerifyBytes(kRvaFindInstanceByKey, kFindInstanceByKeySig, sizeof(kFindInstanceByKeySig))) {
            std::uint64_t rootKey = 0;
            reinterpret_cast<std::uint64_t*(__fastcall*)(std::uint64_t*, unsigned)>(base + kRvaUnitObjectKey)(&rootKey, unsigned(unit));
            root = reinterpret_cast<void*(__fastcall*)(std::uint64_t, bool*)>(base + kRvaFindInstanceByKey)(rootKey, nullptr) ? 1 : 0;
        }
        int written = snprintf(out, capacity, "ready %d, flag %d, movie %d, blocks %d/%d, unit %d %s (root %s), unit +0x30 %d %s, "
                               "%d actors:", ready ? 1 : 0,
                               handler[0x348], reinterpret_cast<MovieFn>(base + kRvaMovieIsPlaying)() ? 1 : 0,
                               *reinterpret_cast<std::uint8_t*>(base + kRvaReadyBlockA),
                               *reinterpret_cast<std::uint8_t*>(base + kRvaReadyBlockB), unit, unitName ? unitName : "?",
                               root < 0 ? "not checked" : (root ? "there" : "MISSING"), otherUnit, otherName ? otherName : "?",
                               count);
        int born = 0, queued = 0;
        for (int i = 0; i < count && actors && written > 0 && std::size_t(written) < capacity; ++i) {
            const std::uint64_t key = *reinterpret_cast<std::uint64_t*>(actors + std::size_t(i) * 0xF0);
            const std::int64_t signedKey = static_cast<std::int64_t>(key);
            if (signedKey < 0 && signedKey > -256) {
                void* instance = special ? special(static_cast<int>(signedKey), -1) : nullptr;
                const bool player = instance && ((*reinterpret_cast<std::uint32_t*>(static_cast<std::uint8_t*>(instance) + 0xC8) >> 5) & 1);
                written += snprintf(out + written, capacity - std::size_t(written), " [%d special %lld: %s]", i,
                                    static_cast<long long>(signedKey), !instance ? "NULL" : (player ? "player" : "found"));
                continue;
            }
            std::uint64_t placement = 0;
            void* instance = reinterpret_cast<ActorFn>(base + kRvaGetCineActor)(handler, key, *reinterpret_cast<int*>(handler + 0x2C), &placement);
            if (instance) {
                ++born;
                continue;
            }
            if (reinterpret_cast<QueuedFn>(base + kRvaIsQueued)(placement, true, true, true)) {
                ++queued;
                continue;
            }
            written += snprintf(out + written, capacity - std::size_t(written), " [%d %llX MISSING]", i,
                                static_cast<unsigned long long>(key));
        }
        if (written > 0 && std::size_t(written) < capacity)
            snprintf(out + written, capacity - std::size_t(written), " (%d born, %d queued)", born, queued);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        snprintf(out, capacity, "(fault while reading the actors)");
    }
}

int CineFrame() noexcept {
    __try {
        return g_impl ? g_cineFrame(&g_impl) : -1;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
}

void StopCine() noexcept {
    __try {
        if (g_impl) g_cineStop(&g_impl);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

// ---- the level scripts' cutscene nodes ----
// A story cutscene is played by its level's action graph: a Cine_PlayCinematic node (TR2 PDB; retail ctor 0x78A870,
// dtor 0x796250, vtable 0xD75E18, ProcessMsg 0x75B1D0 = vtable +8). The node holds its cutscene as +0x7C index (short)
// and +0x7E unit ID (short; params +0x34 / +0x30, input 0 may change them), the NtCinematic at +0x80, +0x88 / +0x89
// play pending / playing. ProcessMsg(node, 2, null) handles the inputs that fired, the bits of +0x14: bit 3 = Play
// (AreAllLocalPlayersDead, SetCine, InitCineParams: controller, UI cine mode, camera; NtCinematic::Play(true)), bit 1
// create, bit 4 stop. ActionGraphInstance::ProcessMessageQueues (retail 0x721EC0) calls it with +0x10 bit 0x20 set
// (busy; skipped when already set), then +0x14 = 0 and the bit cleared once the queued count +0x12 is 0. After that
// the node runs itself (msg 1 each frame) and sends its outputs when the cutscene ends, as when the story plays it.
// Chapter Select starts a few scenes before their cutscene (a checkpoint where Lara walks to it): there the viewer
// fires the node's Play input itself.
constexpr std::uintptr_t kRvaCineNodeCtor = 0x0078A870ull, kRvaCineNodeDtor = 0x00796250ull,
                         kRvaCineNodeVtable = 0x00D75E18ull;
constexpr std::uint8_t kCineNodeCtorSig[] = {0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x74, 0x24, 0x10, 0x57, 0x48,
                                             0x83, 0xEC, 0x50, 0x49, 0x8B, 0xF9};
constexpr std::uint8_t kCineNodeDtorSig[] = {0x40, 0x53, 0x48, 0x83, 0xEC, 0x20, 0x80, 0xB9, 0x92, 0x00, 0x00, 0x00, 0x00};
constexpr std::uint8_t kCineNodeProcessSig[] = {0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x6C, 0x24, 0x18, 0x56,
                                                0x57, 0x41, 0x56, 0x48, 0x83, 0xEC, 0x50, 0x0F, 0xB7, 0x59, 0x7E};
constexpr std::uintptr_t kRvaCineNodeProcess = 0x0075B1D0ull;
using CineNodeCtorFn = void*(__fastcall*)(void*, void*, int, void*);
using CineNodeDtorFn = void(__fastcall*)(void*);
using CineNodeProcessFn = void(__fastcall*)(void*, int, void*);
CineNodeCtorFn g_origCineNodeCtor = nullptr;
CineNodeDtorFn g_origCineNodeDtor = nullptr;
std::mutex g_nodeMutex;
std::vector<void*> g_cineNodes;
bool g_nodeHooks = false;

void* __fastcall HookCineNodeCtor(void* node, void* graph, int nodeEnum, void* data) {
    void* result = g_origCineNodeCtor(node, graph, nodeEnum, data);
    std::lock_guard lock(g_nodeMutex);
    g_cineNodes.push_back(node);
    return result;
}

void __fastcall HookCineNodeDtor(void* node) {
    {
        std::lock_guard lock(g_nodeMutex);
        if (const auto it = std::find(g_cineNodes.begin(), g_cineNodes.end(), node); it != g_cineNodes.end()) {
            *it = g_cineNodes.back();
            g_cineNodes.pop_back();
        }
    }
    g_origCineNodeDtor(node);
}

// The node's cutscene (its unit's list entry), unit and index; false: not a cutscene node now. SEH only.
bool NodeCine(void* node, char* name, std::size_t capacity, int& unit, int& index, int& state) noexcept {
    name[0] = 0;
    __try {
        auto* n = static_cast<std::uint8_t*>(node);
        if (*reinterpret_cast<std::uintptr_t*>(n) != GameBuild::Base() + kRvaCineNodeVtable) return false;
        unit = *reinterpret_cast<std::uint16_t*>(n + 0x7E);
        index = *reinterpret_cast<std::int16_t*>(n + 0x7C);
        state = n[0x88] | (n[0x89] << 1);
        auto* level = static_cast<std::uint8_t*>(g_getLevel(unit));
        if (!level || index < 0) return true;
        auto* unitData = *reinterpret_cast<std::uint8_t**>(level + kLevelUnitDataOffset);
        if (!unitData) return true;
        const std::uint32_t count = *reinterpret_cast<std::uint32_t*>(unitData + kUnitDataCineCountOffset);
        auto** list = *reinterpret_cast<const char***>(unitData + kUnitDataCineListOffset);
        if (list && std::uint32_t(index) < count && list[index]) {
            strncpy_s(name, capacity, list[index], _TRUNCATE);
        }
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        name[0] = 0;
        return false;
    }
}

// Fires the node's Play input as its graph would. 0: fired; 1: busy now (try later); 2: fault.
int FireCineNodePlay(void* node) noexcept {
    __try {
        auto* n = static_cast<std::uint8_t*>(node);
        auto& flags = *reinterpret_cast<std::uint16_t*>(n + 0x10);
        if ((flags & 0x20) || *reinterpret_cast<std::uint16_t*>(n + 0x12) || n[0x88] || n[0x89]) return 1;
        flags = std::uint16_t(flags | 0x20);
        *reinterpret_cast<std::uint32_t*>(n + 0x14) = 1u << 3;
        reinterpret_cast<CineNodeProcessFn>(GameBuild::Base() + kRvaCineNodeProcess)(node, 2, nullptr);
        *reinterpret_cast<std::uint32_t*>(n + 0x14) = 0;
        flags = std::uint16_t(flags & ~0x20);
        return 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 2;
    }
}

// Skips the cutscene the node plays as the player's Cine_Skip does: Cine_PlayCinematic::DoInputAndUIProcess (retail
// 0x7A6C90, TR2 0x7E0AE0) sets +0x8C = 1 while it plays (+0x88); DoCinePlayingUpdateLogic (retail 0x7A6770) then runs
// DoCancelProcess (retail 0x7A6550: a screen wipe, NtCinematic::Stop, output 1 "skipped"), and the end sends output 2
// as a normal end does ("Cinematic %s after %.1f seconds", "skip" or "end"), so the story goes on. 0: asked; 1: not
// playing, or a skip runs already; 2: not a cutscene node / fault.
int SkipCineNode(void* node) noexcept {
    __try {
        auto* n = static_cast<std::uint8_t*>(node);
        if (*reinterpret_cast<std::uintptr_t*>(n) != GameBuild::Base() + kRvaCineNodeVtable) return 2;
        auto& cancel = *reinterpret_cast<std::int32_t*>(n + 0x8C);
        if (!n[0x88] || cancel != 0) return 1;
        cancel = 1;
        return 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 2;
    }
}

// The node's _NtCineImpl (its NtCinematic at +0x80 points to it). SEH only.
void* NodeImpl(void* node) noexcept {
    __try {
        return *reinterpret_cast<void**>(static_cast<std::uint8_t*>(node) + 0x80);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

// ---- the node's place in its action graph ----
// ActionGraphInstance: +0x78 its nodes (ActionGraphNode*[], by index), +0x98 their count. ActionGraphNode: +0x08 its
// graph, +0x10 flags (0x20: no input pending), +0x12 queued messages, +0x14 inputs fired (bits), +0x1A input count,
// +0x1B output count. Before the node object: its input descriptors (16 bytes each; byte +3 low nibble the type, 2 =
// signal), then one pointer per output to its connections, 4 bytes each: u16 target node index, u8 target input, u8
// flags (high nibble set on the last). ActionGraphNode::OutputData (retail 0x720CC0, TR2 0x75A090) delivers an output:
// per connection, ActionGraphInstance::AddMessage(graph, 2, target, input, false) (retail 0x71D6D0) when the target has
// no input pending or flag 0x20, flag 0x20 cleared, the input's bit set in +0x14 (and its data converted). (TR2 PDB;
// retail checked: RemapOutputConnections 0x7230F0, the delivery in 0x720CC0.)
// The story fires a cutscene node's Play input from another node (a trigger, a sequence) whose same output often
// drives more: hiding the gameplay copy of a prop the cutscene animates (Empty Tomb: bp_sarcophagus_01 stayed next to
// bp_prologue_sarcophagus_rig when only the Play input was fired, 0.9.354). So the viewer delivers that whole output.
constexpr std::uintptr_t kRvaAgAddMessage = 0x0071D6D0ull, kRvaAllPlacementsBirthed = 0x0041FA10ull;
constexpr std::uint8_t kAgAddMessageSig[] = {0x48, 0x89, 0x5C, 0x24, 0x18, 0x57, 0x48, 0x83, 0xEC, 0x20, 0x0F, 0xB6,
                                             0x81, 0xC8, 0x00, 0x00, 0x00, 0x48, 0x8B, 0xD9, 0xC0, 0xE8, 0x06, 0x49};
constexpr std::uint8_t kAllPlacementsBirthedSig[] = {0x48, 0x89, 0x6C, 0x24, 0x18, 0x56, 0x48, 0x83, 0xEC, 0x20, 0x0F,
                                                     0xB6, 0xF2, 0x0F, 0xB6, 0xE9};
bool g_agReady = false;
constexpr int kCinePlayInput = 3;

std::uint8_t* AgNode(std::uint8_t* graph, int index) {
    if (index < 0 || index >= *reinterpret_cast<int*>(graph + 0x98)) return nullptr;
    return (*reinterpret_cast<std::uint8_t***>(graph + 0x78))[index];
}
std::uint8_t* AgOutputList(std::uint8_t* node, int output) {
    const int outputs = node[0x1B];
    return output < outputs ? *reinterpret_cast<std::uint8_t**>(node - std::ptrdiff_t(outputs - output) * 8) : nullptr;
}
int AgInputType(std::uint8_t* node, int input) {
    const int inputs = node[0x1A], outputs = node[0x1B];
    if (input >= inputs) return -1;
    return node[std::ptrdiff_t((input - inputs) * 2 - outputs) * 8 + 3] & 0x0F;
}
std::uintptr_t VtableRva(std::uint8_t* node) { return *reinterpret_cast<std::uintptr_t*>(node) - GameBuild::Base(); }

// Appends "who feeds node `target` (input `input`, -1 = any)" to the text; returns the first source and its output.
int AgSources(std::uint8_t* graph, int target, int input, int& firstSource, int& firstOutput, char* text, std::size_t capacity,
              int& written) {
    firstSource = firstOutput = -1;
    int found = 0;
    const int count = *reinterpret_cast<int*>(graph + 0x98);
    for (int index = 0; index < count; ++index) {
        std::uint8_t* node = AgNode(graph, index);
        if (!node) continue;
        for (int output = 0; output < node[0x1B]; ++output) {
            std::uint8_t* entry = AgOutputList(node, output);
            for (int n = 0; entry && n < 256; ++n, entry += 4) {
                const int to = *reinterpret_cast<std::uint16_t*>(entry), at = entry[2];
                if (to == target && (input < 0 || at == input)) {
                    if (!found++) {
                        firstSource = index;
                        firstOutput = output;
                    }
                    if (written >= 0 && std::size_t(written) < capacity)
                        written += snprintf(text + written, capacity - std::size_t(written), " [node %d (vt 0x%llX) out %d -> in %d]",
                                            index, static_cast<unsigned long long>(VtableRva(node)), output, at);
                }
                if (entry[3] & 0xF0) break;
            }
        }
    }
    return found;
}

// Nodes that do the story's work on the way to the scene and pass the signal on (their output 0 after it): the
// signal goes into the earliest of them, as the story's does, instead of out of the last (0.9.377: Locating the
// Source's graph is AGGraphicsOverrideNode -> AGInstanceGiveBirth -> its Cine_PlayCinematic; fired out of the
// GiveBirth, the scene played without the override and the birth: white around its actors). RTTI of the retail
// vtables: .?AVAGGraphicsOverrideNode@@ 0xD6C730, .?AVAGInstanceGiveBirth@@ 0xD6FCF8.
constexpr std::uintptr_t kRvaGraphicsOverrideNodeVtable = 0x00D6C730ull, kRvaInstanceGiveBirthVtable = 0x00D6FCF8ull;
bool StoryWorkNode(std::uint8_t* node) {
    const std::uintptr_t vt = VtableRva(node);
    return vt == kRvaGraphicsOverrideNodeVtable || vt == kRvaInstanceGiveBirthVtable;
}

// ---- the story's event from another instance's graph (Entry::storyEvent) ----
// AGInstance_ActionGraphInstanceNode (retail vtable 0xD663D8; TR2 PDB) relays another instance's graph: its PlacementHash
// key at +0x48, registered with ActionGraphManager (pointer at retail 0x2D9AA50) when byte +0x74 is 2 (retail 0x71AAC4,
// RegisterInstanceNodeToMap 0x722E00), the next node relaying the same instance at +0x50. When that instance's graph
// sends output i (ActionGraphInstanceOutputsNode::ProcessMsg, retail 0x755C40, TR2 0x768300: each input i fired), the
// manager's list for its key (GetInstanceNodeListFromMap(manager, key, manager + 0x20), retail 0x720400, called at
// 0x755CC5; the manager read at 0x755CB6) gets ForwardDataToOutput(i, data, type), vtable +0x28 (the relay's: a jump to
// ActionGraphNode::OutputData): each relay sends output i in its own graph. The story's output 8 of "Mini Boss - Spawn"
// was a signal (the trace records type 2 only); a signal's data is not read (ActionGraph_ConvertActionGraphTypes writes
// the target type's "true" for a signal source), so none is passed.
constexpr std::uintptr_t kRvaAgRelayNodeVtable = 0x00D663D8ull, kRvaAgManagerPtr = 0x02D9AA50ull,
                         kRvaAgRelaysOf = 0x00720400ull, kRvaAgManagerRef = 0x00755CB6ull, kRvaAgRelaysCall = 0x00755CC5ull;
bool g_storyEventReady = false;
constexpr int kMaxNodeScan = 4096;
struct EventRelay {
    std::uint64_t graphId;
    int index;
    std::uintptr_t vt;
};
// AGTimeDelayNode (retail vtable 0xD6EBA0, its RTTI; ProcessMsg 0x73C4D0 at vtable +8, as TR2's 0x79C5E0): Activate
// (message 2) with input 0 or 1 starts it: an ActiveState at +0x48 whose timer (+0x40 s; x30 when byte +0x45 is 0, the
// timer counting the game's 30 Hz frames) is registered with ActionGraphTimerManager; input 3 resets it (the state
// deleted, its timer unregistered). When the timer is up the node is processed each frame, and Update (message 1) sends
// output 0 (its input's value, +0x20, a bool), stops being processed each frame and deletes the state (unregistering
// the timer). ActionGraphNode::OutputData (retail 0x720CC0, TR2 0x75A090): the relay's ForwardDataToOutput (its vtable
// +0x28, retail 0x71AA90) is a jump to it.
constexpr std::uintptr_t kRvaTimeDelayNodeVtable = 0x00D6EBA0ull, kRvaTimeDelayProcessMsg = 0x0073C4D0ull,
                         kRvaAgOutputData = 0x00720CC0ull;
constexpr std::uint8_t kTimeDelayProcessMsgSig[] = {0x40, 0x53, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x8B, 0xD9, 0x85, 0xD2,
                                                    0x75, 0x1C, 0xF6, 0x41, 0x14, 0x01};
constexpr std::uint8_t kAgOutputDataSig[] = {0x4C, 0x8B, 0xDC, 0x41, 0x54, 0x41, 0x55, 0x41, 0x57, 0x48, 0x83, 0xEC, 0x50,
                                             0x0F, 0xB6, 0x41, 0x1B};
bool g_storyDelaysReady = false;
struct RanDelay {
    std::uint64_t graphId;
    int index;
    float seconds;
};

// The relays of the instance that node `index` of the cutscene node's graph relays (the list first: a relay's output
// may change it). Their count, or -1 not a relay / output out of range, -2 none registered, -3 fault.
int StoryRelays(void* cineNode, int index, int output, std::uint64_t& key, std::uint8_t** list, int capacity) noexcept {
    key = 0;
    __try {
        auto* graph = *reinterpret_cast<std::uint8_t**>(static_cast<std::uint8_t*>(cineNode) + 8);
        std::uint8_t* node = graph ? AgNode(graph, index) : nullptr;
        if (!node || VtableRva(node) != kRvaAgRelayNodeVtable || output < 0 || output >= node[0x1B]) return -1;
        key = *reinterpret_cast<std::uint64_t*>(node + 0x48);
        void* manager = *reinterpret_cast<void**>(GameBuild::Base() + kRvaAgManagerPtr);
        if (!manager || !key) return -2;
        using RelaysOfFn = std::uint8_t*(__fastcall*)(void*, std::uint64_t, void*);
        std::uint8_t* relay = reinterpret_cast<RelaysOfFn>(GameBuild::Base() + kRvaAgRelaysOf)(
            manager, key, static_cast<std::uint8_t*>(manager) + 0x20);
        int count = 0;
        for (; relay && count < capacity; relay = *reinterpret_cast<std::uint8_t**>(relay + 0x50)) list[count++] = relay;
        return count ? count : -2;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -3;
    }
}

// The level's delays from its start, run out before the story event: every AGTimeDelayNode running (its ActiveState
// there) in the graphs the event reaches through the relays, but the cutscene's own graph, gets Update as when its timer
// is up (its output sent, its timer gone). The delays run out (their count; `ran` the first of them), or < 0 as
// StoryRelays.
int RunOutStoryDelays(void* cineNode, int index, int output, RanDelay* ran, int capacity) noexcept {
    std::uint64_t key = 0;
    std::uint8_t* list[64];
    const int count = StoryRelays(cineNode, index, output, key, list, 64);
    if (count <= 0) return count;
    __try {
        auto* own = *reinterpret_cast<std::uint8_t**>(static_cast<std::uint8_t*>(cineNode) + 8);
        std::uint8_t* graphs[64];
        int graphCount = 0;
        for (int i = 0; i < count; ++i) {
            auto* g = *reinterpret_cast<std::uint8_t**>(list[i] + 8);
            if (!g || g == own || std::find(graphs, graphs + graphCount, g) != graphs + graphCount) continue;
            graphs[graphCount++] = g;
        }
        using ProcessFn = void(__fastcall*)(void*, int, void*);
        const auto process = reinterpret_cast<ProcessFn>(GameBuild::Base() + kRvaTimeDelayProcessMsg);
        int n = 0;
        for (int i = 0; i < graphCount; ++i) {
            std::uint8_t* g = graphs[i];
            const int nodes = std::min(*reinterpret_cast<int*>(g + 0x98), kMaxNodeScan);
            for (int k = 0; k < nodes; ++k) {
                std::uint8_t* node = AgNode(g, k);
                if (!node || VtableRva(node) != kRvaTimeDelayNodeVtable || !*reinterpret_cast<void**>(node + 0x48)) continue;
                if (n < capacity) ran[n] = {*reinterpret_cast<std::uint64_t*>(g + 0xC0), k, *reinterpret_cast<float*>(node + 0x40)};
                ++n;
                process(node, 1, nullptr);
            }
        }
        return n;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -3;
    }
}

// Sends output `output` of the graph node `index` of the cutscene node's graph relays, to every relay of that instance.
// The cutscene's own relay (that node) whose output leads only into AGTimeDelayNode start inputs (Retreat: node 49's
// output 8 -> node 50, 3 s, -> 51 -> the story's way to the scene: 5.0 s from the event to the scene in 0.9.384's log)
// sends nothing: those delays send their output at once (OutputData(delay, 0, a signal), as the relay's forward does;
// the target's bool input gets "true"), never started, so the story's way begins without its wait. `skipped` gets those
// delays (index, seconds). The relays reached (their count), or < 0 as StoryRelays.
int SendStoryEvent(void* cineNode, int index, int output, std::uint64_t& key, EventRelay* relays, int capacity,
                   RanDelay* skipped, int skipCapacity, int& skippedCount) noexcept {
    skippedCount = 0;
    std::uint8_t* list[64];
    const int count = StoryRelays(cineNode, index, output, key, list, 64);
    if (count <= 0) return count;
    __try {
        auto* own = *reinterpret_cast<std::uint8_t**>(static_cast<std::uint8_t*>(cineNode) + 8);
        std::uint8_t* self = AgNode(own, index);
        using ForwardFn = void(__fastcall*)(void*, int, const void*, int);
        using OutputFn = void(__fastcall*)(void*, int, const void*, int);
        const auto outputData = reinterpret_cast<OutputFn>(GameBuild::Base() + kRvaAgOutputData);
        for (int i = 0; i < count; ++i) {
            std::uint8_t* r = list[i];
            auto* g = *reinterpret_cast<std::uint8_t**>(r + 8);
            int at = -1;
            for (int k = 0; g && k < *reinterpret_cast<int*>(g + 0x98) && k < kMaxNodeScan; ++k)
                if (AgNode(g, k) == r) at = k;
            if (i < capacity) relays[i] = {g ? *reinterpret_cast<std::uint64_t*>(g + 0xC0) : 0ull, at, VtableRva(r)};
            if (r == self && g_storyDelaysReady) {
                int delays[8], n = 0;
                bool onlyDelays = true;
                std::uint8_t* entry = AgOutputList(r, output);
                for (int k = 0; entry && k < 256; ++k, entry += 4) {
                    std::uint8_t* target = AgNode(own, *reinterpret_cast<std::uint16_t*>(entry));
                    if (!target || VtableRva(target) != kRvaTimeDelayNodeVtable || entry[2] > 1 || n == 8) {
                        onlyDelays = false;
                        break;
                    }
                    delays[n++] = *reinterpret_cast<std::uint16_t*>(entry);
                    if (entry[3] & 0xF0) break;
                }
                if (onlyDelays && n) {
                    for (int k = 0; k < n; ++k) {
                        std::uint8_t* delay = AgNode(own, delays[k]);
                        if (skippedCount < skipCapacity)
                            skipped[skippedCount] = {0ull, delays[k], *reinterpret_cast<float*>(delay + 0x40)};
                        ++skippedCount;
                        outputData(delay, 0, nullptr, 2);
                    }
                    continue;
                }
            }
            const auto forward = reinterpret_cast<ForwardFn>((*reinterpret_cast<std::uintptr_t**>(r))[5]);
            forward(r, output, nullptr, 2);
        }
        return count;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -3;
    }
}

// The cutscene's storyEvent read ("<node>:<output>"); false logged.
bool StoryEventOf(int& index, int& output) {
    index = output = -1;
    if (std::sscanf(g_playStoryEvent.c_str(), "%d:%d", &index, &output) != 2) {
        Log::Writef("Cutscenes: %s: its storyEvent \"%s\" is not \"<node>:<output>\"; its node is fired as before", g_playId.c_str(),
                    g_playStoryEvent.c_str());
        return false;
    }
    if (!g_storyEventReady) {
        Log::Writef("Cutscenes: %s: the action graph manager is not where expected; its node is fired as before", g_playId.c_str());
        return false;
    }
    return true;
}
const char* StoryRelaysError(int code) {
    return code == -1 ? "that node is not a relay of another instance's graph, or has no such output"
                      : (code == -2 ? "no relay registered for that instance" : "fault");
}

// The level's delays in the graphs the cutscene's story event reaches run out, logged: their count (0: none running;
// < 0: not done, the event is sent without it).
int RunOutStoryDelaysFor(void* cineNode, int round) {
    int index = -1, output = -1;
    if (!StoryEventOf(index, output)) return -1;
    if (!g_storyDelaysReady) {
        if (round == 0)
            Log::Writef("Cutscenes: %s: AGTimeDelayNode::ProcessMsg is not where expected; the level's delays are not run out "
                        "before its story event", g_playId.c_str());
        return -1;
    }
    RanDelay ran[24];
    const int n = RunOutStoryDelays(cineNode, index, output, ran, 24);
    if (n < 0) {
        Log::Writef("Cutscenes: %s: the level's delays not run out before its story event (%s)", g_playId.c_str(), StoryRelaysError(n));
        return n;
    }
    if (n == 0) {
        Log::Writef("Cutscenes: %s: %s in the graphs its story event reaches", g_playId.c_str(),
                    round == 0 ? "no delay of the level running" : "no more delays running");
        return 0;
    }
    // (graph ID's high half, node, its time; 16 at most: the line stays under the log's 1024 characters)
    std::string list;
    for (int i = 0; i < n && i < 16; ++i) {
        char one[64];
        snprintf(one, sizeof(one), "%s[0x%llX:%d %.1f s]", i ? " " : "", static_cast<unsigned long long>(ran[i].graphId >> 32),
                 ran[i].index, double(ran[i].seconds));
        list += one;
    }
    if (n > 16) list += " ...";
    Log::Writef("Cutscenes: %s: %d delay%s of the level run out in the graphs its story event reaches (round %d; graph ID's high "
                "half:node, its time): %s; the event %.1f s later at the earliest", g_playId.c_str(), n, n == 1 ? "" : "s",
                round + 1, list.c_str(), kStoryRoundMs / 1000.0);
    return n;
}

// The cutscene's story event sent (true), logged.
bool SendStoryEventFor(void* cineNode) {
    int index = -1, output = -1;
    if (!StoryEventOf(index, output)) return false;
    std::uint64_t key = 0;
    EventRelay relays[64];
    RanDelay skipped[8];
    int skippedCount = 0;
    const int sent = SendStoryEvent(cineNode, index, output, key, relays, 64, skipped, 8, skippedCount);
    if (sent <= 0) {
        Log::Writef("Cutscenes: %s: its story event (output %d of node %d) not sent (%s); its node is fired as before",
                    g_playId.c_str(), output, index, StoryRelaysError(sent));
        return false;
    }
    // (by graph: "[graph 0x62B0141100000000: 13 3]"; the line stays under the log's 1024 characters)
    std::string list;
    bool done[64] = {};
    int graphs = 0;
    for (int i = 0; i < sent && i < 64; ++i) {
        if (done[i]) continue;
        ++graphs;
        char one[64];
        snprintf(one, sizeof(one), "%s[0x%llX:", list.empty() ? "" : " ", static_cast<unsigned long long>(relays[i].graphId >> 32));
        list += one;
        for (int k = i; k < sent && k < 64; ++k) {
            if (done[k] || relays[k].graphId != relays[i].graphId) continue;
            done[k] = true;
            snprintf(one, sizeof(one), " %d", relays[k].index);
            list += one;
        }
        list += "]";
    }
    std::string skip;
    for (int i = 0; i < skippedCount && i < 8; ++i) {
        char one[64];
        snprintf(one, sizeof(one), "%snode %d (%.1f s)", i ? ", " : "", skipped[i].index, double(skipped[i].seconds));
        skip += one;
    }
    if (list.size() > 600) list = list.substr(0, 600) + " ...";
    Log::Writef("Cutscenes: %s: the story's event sent: output %d of node %d (the instance 0x%llX's graph) to its %d relays in %d "
                "graphs (graph ID high half: relay nodes): %s%s%s; the scene comes the story's way (its node fired directly if it "
                "has not come in 15 s)", g_playId.c_str(), output, index, static_cast<unsigned long long>(key), sent, graphs,
                list.c_str(), skip.empty() ? "" : "; the delay after its own relay passed at once: ", skip.c_str());
    return true;
}

// The cutscene node's Play input fired through the output that drives it in its graph (every signal input on that
// output gets it, as OutputData would deliver it); when that output is a StoryWorkNode's, the signal goes into the
// earliest such node of the chain instead (the input its feeder drives). 0 delivered; 1 busy; 2 fault; 3 nothing drives
// it (or no graph).
int FireCinePlayThroughGraph(void* cineNode, char* text, std::size_t capacity) noexcept {
    text[0] = 0;
    __try {
        auto* node = static_cast<std::uint8_t*>(cineNode);
        if (!g_agReady) return 3;
        if ((*reinterpret_cast<std::uint16_t*>(node + 0x10) & 0x20) == 0 && *reinterpret_cast<std::uint32_t*>(node + 0x14)) return 1;
        if (*reinterpret_cast<std::uint16_t*>(node + 0x12) || node[0x88] || node[0x89]) return 1;
        auto* graph = *reinterpret_cast<std::uint8_t**>(node + 8);
        if (!graph) return 3;
        int self = -1;
        const int count = *reinterpret_cast<int*>(graph + 0x98);
        for (int index = 0; index < count && self < 0; ++index)
            if (AgNode(graph, index) == node) self = index;
        if (self < 0) return 3;
        int written = snprintf(text, capacity, "node %d of %d; Play fed by:", self, count);
        int source = -1, output = -1;
        if (!AgSources(graph, self, kCinePlayInput, source, output, text, capacity, written)) return 3;
        std::uint8_t* from = AgNode(graph, source);
        using AddMessageFn = void(__fastcall*)(void*, int, int, int, bool);
        auto addMessage = reinterpret_cast<AddMessageFn>(GameBuild::Base() + kRvaAgAddMessage);
        // back through the story's work on the way (4 nodes at most): the earliest one's signal input that the graph feeds
        int entryNode = -1, entryInput = -1, walk = source;
        for (int depth = 0; depth < 4 && walk >= 0; ++depth) {
            std::uint8_t* n = AgNode(graph, walk);
            if (!n || !StoryWorkNode(n)) break;
            int feeder = -1, fedInput = -1;
            for (int in = 0; in < n[0x1A] && fedInput < 0; ++in) {
                if (AgInputType(n, in) != 2) continue;
                int src = -1, srcOut = -1, none = -1;
                if (AgSources(graph, walk, in, src, srcOut, text, capacity, none)) {
                    feeder = src;
                    fedInput = in;
                }
            }
            if (fedInput < 0) break;
            entryNode = walk;
            entryInput = fedInput;
            walk = feeder;
        }
        if (entryNode >= 0) {
            std::uint8_t* target = AgNode(graph, entryNode);
            auto& flags = *reinterpret_cast<std::uint16_t*>(target + 0x10);
            auto& fired = *reinterpret_cast<std::uint32_t*>(target + 0x14);
            if (fired == 0 || (flags & 0x20)) addMessage(graph, 2, entryNode, entryInput, false);
            flags = std::uint16_t(flags & 0xFFDF);
            fired |= 1u << entryInput;
            if (std::size_t(written) < capacity)
                written += snprintf(text + written, capacity - std::size_t(written),
                                    "; the story's work on the way: fired into node %d (vt 0x%llX) in %d (fed by node %d)",
                                    entryNode, static_cast<unsigned long long>(VtableRva(target)), entryInput, walk);
            return 0;
        }
        if (std::size_t(written) < capacity)
            written += snprintf(text + written, capacity - std::size_t(written), "; node %d out %d delivers to:", source, output);
        bool hitSelf = false;
        std::uint8_t* entry = AgOutputList(from, output);
        for (int n = 0; entry && n < 256; ++n, entry += 4) {
            const int to = *reinterpret_cast<std::uint16_t*>(entry), at = entry[2];
            std::uint8_t* target = AgNode(graph, to);
            const int type = target ? AgInputType(target, at) : -1;
            if (target && type == 2) {   // (signals only: the data inputs would need the source's value)
                auto& flags = *reinterpret_cast<std::uint16_t*>(target + 0x10);
                auto& fired = *reinterpret_cast<std::uint32_t*>(target + 0x14);
                if (fired == 0 || (flags & 0x20)) addMessage(graph, 2, to, at, false);
                flags = std::uint16_t(flags & 0xFFDF);
                fired |= 1u << at;
                hitSelf |= to == self && at == kCinePlayInput;
            }
            if (std::size_t(written) < capacity)
                written += snprintf(text + written, capacity - std::size_t(written), " [node %d (vt 0x%llX) in %d%s]", to,
                                    target ? static_cast<unsigned long long>(VtableRva(target)) : 0ull, at,
                                    type == 2 ? "" : " not a signal: left");
            if (entry[3] & 0xF0) break;
        }
        // one level more, for the log: who feeds the source
        int up = -1, upOutput = -1;
        if (std::size_t(written) < capacity)
            written += snprintf(text + written, capacity - std::size_t(written), "; node %d (vt 0x%llX) fed by:", source,
                                static_cast<unsigned long long>(VtableRva(from)));
        if (!AgSources(graph, source, -1, up, upOutput, text, capacity, written) && std::size_t(written) < capacity)
            snprintf(text + written, capacity - std::size_t(written), " nothing");
        return hitSelf ? 0 : 3;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 2;
    }
}

// ---- the story's layer switches on the way to the scene ----
// The cutscene node's graph may switch stream layers itself: UnitStreamLayerNode (retail vtable 0xD72490, ProcessMsg
// 0x74F940; TR2 0x76C7E0, its data +0 the unit (u16), +4 the layer: +0x20 / +0x24 in the node) turns layer +0x24 of
// unit +0x20 on with input 0 (ActivateStreamLayer, then output 0), off with input 1 (output 1), asks whether it is on
// with input 2 (output 2, the answer), loads it hidden with inputs 3 / 5 (LoadAndHideStreamLayer; outputs 3, 5, 6).
// Walking back from the cutscene node's Play input through every connection (kStoryWalkDepth steps), the layer nodes
// met through output 0 or 2 had their layer on before the scene: those layers are turned on as well (A Connection:
// its props did not show in 0.9.361 with its Cine_Props_Scene_23_A_Connection layer on). Every layer node of the
// graph is logged, for what the walk does not reach.
constexpr std::uintptr_t kRvaUnitStreamLayerNodeVtable = 0x00D72490ull, kRvaUnitStreamLayerNodeProcess = 0x0074F940ull;
constexpr int kMaxGraphNodes = 4096, kStoryWalkDepth = 8, kMaxLayerSwitches = 64;
struct LayerSwitch {
    int node, unit, layer, output;   // output: the one through which it leads to the scene (-1: it does not)
    unsigned fed;                    // its inputs the graph's links feed (bit 0: activate, bit 1: deactivate)
};
std::int16_t g_walkDepth[kMaxGraphNodes];   // (game thread)

int GraphLayerSwitches(void* cineNode, LayerSwitch* out, int capacity) noexcept {
    __try {
        auto* node = static_cast<std::uint8_t*>(cineNode);
        auto* graph = *reinterpret_cast<std::uint8_t**>(node + 8);
        if (!graph) return -1;
        const int count = *reinterpret_cast<int*>(graph + 0x98);
        if (count <= 0 || count > kMaxGraphNodes) return -1;
        int self = -1;
        for (int i = 0; i < count; ++i) {
            g_walkDepth[i] = -1;
            if (AgNode(graph, i) == node) self = i;
        }
        if (self < 0) return -1;
        const std::uintptr_t vtable = GameBuild::Base() + kRvaUnitStreamLayerNodeVtable;
        int found = 0;
        for (int i = 0; i < count && found < capacity; ++i) {
            std::uint8_t* n = AgNode(graph, i);
            if (!n || *reinterpret_cast<std::uintptr_t*>(n) != vtable) continue;
            out[found++] = {i, *reinterpret_cast<int*>(n + 0x20), *reinterpret_cast<int*>(n + 0x24), -1, 0u};
        }
        // (which of their inputs are linked: every link of the graph)
        for (int source = 0; source < count && found; ++source) {
            std::uint8_t* from = AgNode(graph, source);
            if (!from) continue;
            for (int output = 0; output < from[0x1B]; ++output) {
                std::uint8_t* entry = AgOutputList(from, output);
                for (int k = 0; entry && k < 256; ++k, entry += 4) {
                    const int to = *reinterpret_cast<std::uint16_t*>(entry), at = entry[2];
                    for (int f = 0; f < found; ++f)
                        if (out[f].node == to && at < 32) out[f].fed |= 1u << at;
                    if (entry[3] & 0xF0) break;
                }
            }
        }
        g_walkDepth[self] = 0;
        for (int depth = 0; depth < kStoryWalkDepth; ++depth)
            for (int source = 0; source < count; ++source) {
                std::uint8_t* from = AgNode(graph, source);
                if (!from) continue;
                for (int output = 0; output < from[0x1B]; ++output) {
                    std::uint8_t* entry = AgOutputList(from, output);
                    for (int k = 0; entry && k < 256; ++k, entry += 4) {
                        const int to = *reinterpret_cast<std::uint16_t*>(entry), at = entry[2];
                        if (to < count && g_walkDepth[to] == depth && (depth > 0 || at == kCinePlayInput)) {
                            if (g_walkDepth[source] < 0) g_walkDepth[source] = std::int16_t(depth + 1);
                            for (int f = 0; f < found; ++f)
                                if (out[f].node == source && out[f].output < 0) out[f].output = output;
                        }
                        if (entry[3] & 0xF0) break;
                    }
                }
            }
        return found;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
}

// A layer's name (false: the unit's data is not there or no such layer). SEH only.
bool LayerName(int unitId, int layer, char* out, std::size_t capacity) noexcept {
    out[0] = 0;
    __try {
        auto* level = static_cast<std::uint8_t*>(g_getLevel(unitId));
        auto* unitData = level ? *reinterpret_cast<std::uint8_t**>(level + kLevelUnitDataOffset) : nullptr;
        if (!unitData || layer < 0) return false;
        const std::uint32_t count = *reinterpret_cast<std::uint32_t*>(unitData + kUnitDataLayerCountOffset);
        auto* list = *reinterpret_cast<std::uint8_t**>(unitData + kUnitDataLayerListOffset);
        if (!list || std::uint32_t(layer) >= count) return false;
        const char* name = *reinterpret_cast<const char**>(list + std::size_t(layer) * kLayerEntrySize);
        std::size_t k = 0;
        for (; name && k + 1 < capacity && name[k]; ++k) out[k] = name[k];
        out[k] = 0;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        out[0] = 0;
        return false;
    }
}

bool AllPlacementsBirthed() noexcept {
    __try {
        return reinterpret_cast<bool(__fastcall*)(bool, bool)>(GameBuild::Base() + kRvaAllPlacementsBirthed)(true, false);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return true;
    }
}

struct NodeFind {
    void* node = nullptr;
    int unit = -1, index = -1, state = 0, total = 0;
};
NodeFind FindCineNode(const std::string& id) {
    std::vector<void*> nodes;
    {
        std::lock_guard lock(g_nodeMutex);
        nodes = g_cineNodes;
    }
    NodeFind found;
    for (void* node : nodes) {
        char name[128];
        int unit = -1, index = -1, state = 0;
        if (!NodeCine(node, name, sizeof(name), unit, index, state)) continue;
        ++found.total;
        if (!found.node && name[0] && SameCine(name, id)) {
            found.node = node;
            found.unit = unit;
            found.index = index;
            found.state = state;
        }
    }
    return found;
}

// Viewer: another cutscene the level plays before ours (Long Journey's start plays 14_expedtion_a first; The Enemy's
// igc_bear_valley_go_to_sleep; Execution's way: The Reckoning) is skipped through its node, every 2 s while it still
// plays (3 tries): the story goes on to ours sooner, behind the viewer's black.
// (from 1 s in: the player's skip waits for the streams, the render prewarm and, for some cutscenes, a minimum time
// (Cine_PlayCinematic::DoInputAndUIProcess); Execution's way there, The Reckoning, skipped on its first frames in
// 0.9.360, led on to A New Beginning only the second time it was started)
void SkipOtherCine(const char* name, float frame, ULONGLONG now) {
    const std::string bare = BareCine(name);
    if (bare != g_skipName) {
        g_skipName = bare;
        g_skipTries = 0;
        g_skipAt = 0;
    }
    if (frame < kCineFps || g_skipTries >= 3 || now < g_skipAt) return;
    g_skipAt = now + 2000;
    ++g_skipTries;
    const NodeFind find = FindCineNode(bare);
    if (!find.node) {
        if (g_skipTries == 1) Log::Writef("Cutscenes: %s plays; no node of it is loaded, so it is not skipped", bare.c_str());
        return;
    }
    const int result = SkipCineNode(find.node);
    if (result == 0) Log::Writef("Cutscenes: %s skipped (the viewer waits for %s)", bare.c_str(), g_playId.c_str());
    else if (result == 2 && g_skipTries == 1) Log::Writef("Cutscenes: %s could not be skipped", bare.c_str());
}

// Watching: the layers of ours in a unit that may play it, once per unit (ours may play in a unit the story loads
// later: Execution's).
// The cutscene's "layers" (Entry::layers) that the unit has, turned on, then its "layersOff" turned off. The count
// turned on.
std::vector<std::string> g_playLayers, g_playLayersOff, g_playUnits;
// A "layers" / "layersOff" name for this unit: the layer's name (unqualified names: any unit looked in; "<unit>:<layer>":
// that unit's only), or "" when it is not for it.
std::string NameForUnit(const std::string& entry, const char* unitName) {
    const auto colon = entry.find(':');
    if (colon == std::string::npos) return entry;
    return unitName && _stricmp(entry.substr(0, colon).c_str(), unitName) == 0 ? entry.substr(colon + 1) : std::string();
}
int ActivateNamedLayers(int unitId) {
    if ((g_playLayers.empty() && g_playLayersOff.empty()) || !g_layersReady) return 0;
    const char* unitName = ZoneList::Name(std::uint32_t(unitId));
    const int count = UnitLayerNames(unitId);
    std::string text;
    int on = 0;
    for (int i = 0; i < count; ++i)
        for (const std::string& entry : g_playLayers)
            if (const std::string name = NameForUnit(entry, unitName); !name.empty() && _stricmp(g_layerNames[i], name.c_str()) == 0) {
                const bool ok = TurnOnLayer(unitId, i);
                on += ok;
                text += (text.empty() ? "" : ", ") + name + " (" + std::to_string(i) + (ok ? ")" : ", FAILED)");
            }
    if (!text.empty())
        Log::Writef("Cutscenes: %s: its layers from cutscenes.json turned on in unit %d: %s", g_playId.c_str(), unitId,
                    text.c_str());
    std::string offText;
    for (int i = 0; i < count; ++i)
        for (const std::string& entry : g_playLayersOff)
            if (const std::string name = NameForUnit(entry, unitName); !name.empty() && _stricmp(g_layerNames[i], name.c_str()) == 0) {
                const bool ok = TurnOffLayer(unitId, i);
                offText += (offText.empty() ? "" : ", ") + name + " (" + std::to_string(i) + (ok ? ")" : ", FAILED)");
            }
    if (!offText.empty())
        Log::Writef("Cutscenes: %s: its layersOff from cutscenes.json turned off in unit %d: %s", g_playId.c_str(), unitId,
                    offText.c_str());
    return on;
}

void LookForCineLayers(int unitId, ULONGLONG now) {
    if (unitId <= 0 || std::find(g_layerUnits.begin(), g_layerUnits.end(), unitId) != g_layerUnits.end()) return;
    int on = ActivateCineLayers(unitId, g_playId);
    if (on < 0) return;   // (again on the next tick)
    g_layerUnits.push_back(unitId);
    on += ActivateNamedLayers(unitId);
    if (on > 0) {
        g_layersOn += on;
        g_layersAt = now;
        g_birthWaitSince = 0;
    }
}

// STREAM_QueueUnitLoad(unit ID) (retail 0x4232F0, TR2 0x44EDB0: STREAM_QueueUnitLoad(id, 0, 0)): a unit streamed in
// beside the ones loaded, as the action graphs' AGLoadUnitNode does (TR2 0x761AD0, input 2 without its "hard load" flag).
constexpr std::uintptr_t kRvaQueueUnitLoad = 0x004232F0ull;
constexpr std::uint8_t kQueueUnitLoadSig[] = {0x45, 0x33, 0xC0, 0x33, 0xD2, 0xE9, 0x06, 0x00, 0x00, 0x00, 0xCC, 0xCC, 0xCC,
                                              0xCC, 0xCC, 0xCC, 0x40, 0x55, 0x41, 0x54, 0x41, 0x56, 0x41, 0x57};
bool g_unitLoadReady = false;
bool QueueUnitLoad(int unitId) noexcept {
    if (!g_unitLoadReady || unitId <= 0 || unitId > 0xFFFF) return false;
    __try {
        reinterpret_cast<void(__fastcall*)(std::uint16_t)>(GameBuild::Base() + kRvaQueueUnitLoad)(std::uint16_t(unitId));
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
int UnitIdByName(const std::string& name) {
    for (std::uint32_t id = 1, max = ZoneList::MaxId(); id <= max; ++id)
        if (const char* n = ZoneList::Name(id); n && _stricmp(n, name.c_str()) == 0) return int(id);
    return 0;
}
// Watching: the cutscene's "units" streamed in (each queued once), and the layers named for other units ("<unit>:...")
// switched once each is there. True: all of them are there (or not to be had: 60 s after the first was queued).
std::vector<std::string> g_unitsQueued;
ULONGLONG g_unitsQueuedAt = 0;
bool g_unitsTimeoutLogged = false;
bool PlayUnitsReady(ULONGLONG now) {
    bool ready = true;
    for (const std::string& name : g_playUnits) {
        const int loaded = LoadedUnitId(name.c_str());
        if (loaded > 0 && UnitLayerNames(loaded) > 0) continue;
        ready = false;
        if (std::find(g_unitsQueued.begin(), g_unitsQueued.end(), name) != g_unitsQueued.end()) continue;
        g_unitsQueued.push_back(name);
        const int id = UnitIdByName(name);
        const bool queued = id > 0 && QueueUnitLoad(id);
        if (!g_unitsQueuedAt) g_unitsQueuedAt = now;
        Log::Writef("Cutscenes: %s: unit %s (%d) %s", g_playId.c_str(), name.c_str(), id,
                    queued ? "queued to stream in (its \"units\")" : (id ? "NOT queued (the load call did not match)" : "not known"));
    }
    // the layers named for other units, once each such unit is loaded
    for (const auto* list : {&g_playLayers, &g_playLayersOff})
        for (const std::string& entry : *list) {
            const auto colon = entry.find(':');
            if (colon == std::string::npos) continue;
            const int loaded = LoadedUnitId(entry.substr(0, colon).c_str());
            if (loaded <= 0 || UnitLayerNames(loaded) <= 0 ||
                std::find(g_layerUnits.begin(), g_layerUnits.end(), loaded) != g_layerUnits.end())
                continue;
            g_layerUnits.push_back(loaded);
            if (const int on = ActivateNamedLayers(loaded); on > 0) {
                g_layersOn += on;
                g_layersAt = now;
                g_birthWaitSince = 0;
            }
        }
    if (!ready && g_unitsQueuedAt && now - g_unitsQueuedAt > 60000) {
        if (!g_unitsTimeoutLogged) {
            g_unitsTimeoutLogged = true;
            Log::Writef("Cutscenes: %s: its units not all loaded 60 s after they were queued; it is started without them",
                        g_playId.c_str());
        }
        return true;
    }
    return ready;
}

// Watching: the layer switches of the graph of a node about to be fired, once per node.
bool g_storyLayersReady = false;
std::vector<void*> g_storyNodes;
void LookForStoryLayers(void* node, const std::string& cineId, ULONGLONG now) {
    if (!g_storyLayersReady || !g_layersReady || !node ||
        std::find(g_storyNodes.begin(), g_storyNodes.end(), node) != g_storyNodes.end())
        return;
    g_storyNodes.push_back(node);
    LayerSwitch switches[kMaxLayerSwitches];
    const int count = GraphLayerSwitches(node, switches, kMaxLayerSwitches);
    if (count <= 0) {
        Log::Writef("Cutscenes: %s: %s", cineId.c_str(),
                    count == 0 ? "its graph switches no stream layer" : "its graph could not be walked for stream layers");
        return;
    }
    std::string text;
    int on = 0;
    for (int i = 0; i < count; ++i) {
        const LayerSwitch& s = switches[i];
        char name[kLayerNameSize];
        if (!LayerName(s.unit, s.layer, name, sizeof(name))) snprintf(name, sizeof(name), "layer %d", s.layer);
        const bool before = s.output == 0 || s.output == 2;
        // (one elsewhere in the graph linked only to its "activate" input: turned on too. 0.9.364's Locating the Source
        // had pieces missing; its graph has a layer node for gr_010_gulag_rescue's Content_Trees away from its node)
        const bool elsewhere = s.output < 0 && s.fed == 1u;
        const bool turned = (before || elsewhere) && TurnOnLayer(s.unit, s.layer);
        on += turned;
        char item[192];
        if (elsewhere)
            snprintf(item, sizeof(item), "%s%s (unit %d, node %d): turned on elsewhere in it (only), %s", text.empty() ? "" : "; ",
                     name, s.unit, s.node, turned ? "turned on" : "NOT turned on");
        else if (s.output < 0)
            snprintf(item, sizeof(item), "%s%s (unit %d, node %d): not on the way to it (inputs linked: %X)", text.empty() ? "" : "; ",
                     name, s.unit, s.node, s.fed);
        else if (before)
            snprintf(item, sizeof(item), "%s%s (unit %d, node %d): on before it (output %d), %s", text.empty() ? "" : "; ", name,
                     s.unit, s.node, s.output, turned ? "turned on" : "NOT turned on");
        else
            snprintf(item, sizeof(item), "%s%s (unit %d, node %d): on the way to it through output %d (left)",
                     text.empty() ? "" : "; ", name, s.unit, s.node, s.output);
        text += item;
    }
    Log::Writef("Cutscenes: %s: its graph's stream layer nodes: %s", cineId.c_str(), text.c_str());
    if (on) {
        g_layersOn += on;
        g_layersAt = now;
        g_birthWaitSince = 0;
    }
}

// The unit's stream layers that are on, by name (for the log at the cutscene's start): StreamLayerManager::
// IsStreamLayerActive(manager, StreamUnit*, layer) (retail 0x40C9F0, TR2 0x4382C0: the unit's root instance's
// StreamLayerComponent, bit layer + 1). The StreamUnit: the tracker's entry of the unit.
constexpr std::uintptr_t kRvaIsStreamLayerActive = 0x0040C9F0ull;
constexpr std::uint8_t kIsStreamLayerActiveSig[] = {0x48, 0x8B, 0x82, 0x28, 0x02, 0x00, 0x00, 0x45, 0x8B, 0xC8, 0x48,
                                                    0x8B, 0x48, 0x68};
bool g_layerActiveReady = false;
std::uint8_t* StreamUnitOf(int unitId) noexcept {
    __try {
        auto* tracker = reinterpret_cast<std::uint8_t*>(GameBuild::Base() + kRvaStreamTracker);
        for (int i = 0; i < kUnitCount; ++i) {
            std::uint8_t* unit = tracker + std::size_t(i) * kUnitStride;
            const int state = unit[4] & 0x7F;
            if (state != 0 && state != 4 && *reinterpret_cast<const int*>(unit) == unitId) return unit;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    return nullptr;
}
// 1 on, 0 off, -1 not known. SEH only.
int LayerActive(int unitId, int layer) noexcept {
    if (!g_layerActiveReady) return -1;
    std::uint8_t* streamUnit = StreamUnitOf(unitId);
    __try {
        void* manager = reinterpret_cast<void*(__fastcall*)()>(GameBuild::Base() + kRvaStreamLayerManagerGet)();
        if (!streamUnit || !manager) return -1;
        return reinterpret_cast<bool(__fastcall*)(void*, void*, int)>(GameBuild::Base() + kRvaIsStreamLayerActive)(
                   manager, streamUnit, layer)
                   ? 1
                   : 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
}
int ActiveLayerFlags(int unitId, int count, bool* active) noexcept {
    std::uint8_t* streamUnit = StreamUnitOf(unitId);
    __try {
        void* manager = reinterpret_cast<void*(__fastcall*)()>(GameBuild::Base() + kRvaStreamLayerManagerGet)();
        if (!streamUnit || !manager) return 0;
        using IsActiveFn = bool(__fastcall*)(void*, void*, int);
        auto isActive = reinterpret_cast<IsActiveFn>(GameBuild::Base() + kRvaIsStreamLayerActive);
        int on = 0;
        for (int i = 0; i < count; ++i) on += active[i] = isActive(manager, streamUnit, i);
        return on;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
}
bool g_activeLayersLogged = false;
std::string TimeOfDayText(int type);
int ActiveTimeOfDay() noexcept;
// The loaded units (the stream tracker's): their IDs and names, their count. SEH only.
struct LoadedUnit {
    int id;
    char name[64];
};
int LoadedUnits(LoadedUnit* out, int capacity) noexcept {
    int n = 0;
    __try {
        auto* tracker = reinterpret_cast<std::uint8_t*>(GameBuild::Base() + kRvaStreamTracker);
        for (int i = 0; i < kUnitCount && n < capacity; ++i) {
            std::uint8_t* unit = tracker + std::size_t(i) * kUnitStride;
            const int state = unit[4] & 0x7F;
            if (state == 0 || state == 4) continue;
            out[n].id = *reinterpret_cast<const int*>(unit);
            const char* name = reinterpret_cast<const char*>(unit + kUnitNameOffset);
            std::size_t k = 0;
            for (; k + 1 < sizeof(out[n].name) && name[k]; ++k) out[n].name[k] = name[k];
            out[n].name[k] = 0;
            ++n;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    return n;
}

// The scene's state at a cutscene's start (when: "at its start" or "when it did not come"; story: one the game played
// on its own, outside the viewer): the units loaded, each one's stream layers on (logged; every layer of each, on or
// off, to cutscenes\layers\<unit>.txt, <unit>.story.txt for the story's), the time of day and the camera's shakes. A
// scene the viewer plays and the same scene in the story can be compared (0.9.376, Locating the Source: in the viewer
// white, the story's in a lit dome).
void LogSceneState(const std::string& cine, const char* when, bool story) {
    if (!g_layerActiveReady || !g_layersReady) return;
    LoadedUnit units[kUnitCount];
    const int unitCount = LoadedUnits(units, kUnitCount);
    std::string list;
    for (int u = 0; u < unitCount; ++u)
        list += (list.empty() ? "" : ", ") + std::string(units[u].name) + " (" + std::to_string(units[u].id) + ")";
    Log::Detailf("Cutscenes: %s%s: the units loaded %s: %s", cine.c_str(), story ? " (the story's)" : "", when,
                list.empty() ? "none" : list.c_str());
    std::error_code ec;
    const fs::path folder = CatalogPath().parent_path() / L"layers";
    fs::create_directories(folder, ec);
    static bool active[kMaxLayers];
    for (int u = 0; u < unitCount; ++u) {
        const int unitId = units[u].id;
        const int count = UnitLayerNames(unitId);
        if (count <= 0) continue;
        std::fill(active, active + count, false);
        const int on = ActiveLayerFlags(unitId, count, active);
        if (on < 0) continue;
        // (in lines of up to 700 characters: the log's lines end at 1024)
        std::vector<std::string> parts(1);
        for (int i = 0; i < count; ++i) {
            if (!active[i]) continue;
            if (parts.back().size() > 700) parts.emplace_back();
            parts.back() += (parts.back().empty() ? "" : ", ") + std::string(g_layerNames[i]);
        }
        for (std::size_t part = 0; part < parts.size(); ++part)
            Log::Detailf("Cutscenes: %s%s: %s's stream layers on %s (%d of %d%s): %s", cine.c_str(),
                        story ? " (the story's)" : "", units[u].name, when, on, count, part ? ", continued" : "",
                        parts[part].empty() ? "none" : parts[part].c_str());
        // every layer of the unit, on or off (names for cutscenes.json "layers" / "layersOff")
        std::ofstream out(folder / (std::string(units[u].name[0] ? units[u].name : std::to_string(unitId).c_str()) +
                                    (story ? ".story.txt" : ".txt")),
                          std::ios::binary | std::ios::trunc);
        out << "index\tlayer\ton " << when << " (" << cine << (story ? ", the story's" : ", the viewer's") << ")\n";
        for (int i = 0; i < count; ++i) out << i << '\t' << g_layerNames[i] << '\t' << (active[i] ? "on" : "off") << '\n';
    }
    Log::Detailf("Cutscenes: %s%s: the time of day %s: %s", cine.c_str(), story ? " (the story's)" : "", when,
                TimeOfDayText(ActiveTimeOfDay()).c_str());
    std::uint32_t own = 0;
    const int shakes = Features::CameraShakesPlaying(false, &own);
    Log::Detailf("Cutscenes: %s%s: the camera's shakes %s: %d playing, its camera's own shake %u", cine.c_str(),
                story ? " (the story's)" : "", when, shakes, own);
}

// (when: "at its start", or "when it did not come")
void LogActiveLayers(int unitId, const char* when = "at its start") {
    if (g_activeLayersLogged || !g_layerActiveReady || !g_layersReady || unitId <= 0) return;
    g_activeLayersLogged = true;
    LogSceneState(g_playId, when, false);
}

// The cutscene's "layersOffAtStart" (Entry::layersOffAtStart), turned off once, as the game starts it (its first frame
// seen): "<unit>:<layer>" in that loaded unit, a bare name in the start's unit (g_playUnit).
std::vector<std::string> g_playLayersOffAtStart;
bool g_layersOffAtStartDone = false;
void TurnOffLayersAtStart() {
    if (g_layersOffAtStartDone || g_playLayersOffAtStart.empty()) return;
    g_layersOffAtStartDone = true;
    if (!g_layersReady || !g_layersOffReady) {
        Log::Writef("Cutscenes: %s: its layersOffAtStart left on (the layer calls do not match)", g_playId.c_str());
        return;
    }
    LoadedUnit units[kUnitCount];
    const int unitCount = LoadedUnits(units, kUnitCount);
    std::string text;
    int off = 0, named = 0;
    for (int u = 0; u < unitCount; ++u) {
        const int unitId = units[u].id;
        std::vector<std::string> names;
        for (const std::string& entry : g_playLayersOffAtStart) {
            const bool qualified = entry.find(':') != std::string::npos;
            if (!qualified && _stricmp(units[u].name, g_playUnit.c_str()) != 0) continue;
            if (std::string name = NameForUnit(entry, units[u].name); !name.empty()) names.push_back(std::move(name));
        }
        if (names.empty()) continue;
        const int count = UnitLayerNames(unitId);
        std::string unitText;
        for (int i = 0; i < count; ++i)
            for (const std::string& name : names)
                if (_stricmp(g_layerNames[i], name.c_str()) == 0) {
                    ++named;
                    const int was = LayerActive(unitId, i);
                    const bool ok = was == 0 || TurnOffLayer(unitId, i);
                    off += ok && was != 0;
                    unitText += (unitText.empty() ? "" : ", ") + name + " (" + std::to_string(i) +
                                (was == 0 ? ", was off)" : ok ? ")" : ", FAILED)");
                }
        if (!unitText.empty()) text += (text.empty() ? "" : "; ") + std::string(units[u].name) + ": " + unitText;
    }
    Log::Writef("Cutscenes: %s: its layersOffAtStart turned off as it started (%d of %d named; %d found): %s",
                g_playId.c_str(), off, int(g_playLayersOffAtStart.size()), named, text.empty() ? "none found" : text.c_str());
}

// Before the node is fired: are the layers turned on for the scene still on? A respawn can turn them off again (0.9.362:
// Trust Issues' layer, turned on in the moment of the viewer's outfit swap, was off when its node fired; 5 of its 7
// actors MISSING). One that is off is turned on again (3 times at most) and the firing waits for it.
bool LayersStillOn(ULONGLONG now) {
    bool all = true;
    for (TurnedOn& t : g_turnedOn) {
        if (LayerActive(t.unit, t.layer) != 0 || t.tries > 3) continue;   // (on, or not known)
        ++t.tries;
        TurnOnLayer(t.unit, t.layer);
        char name[kLayerNameSize];
        if (!LayerName(t.unit, t.layer, name, sizeof(name))) snprintf(name, sizeof(name), "layer %d", t.layer);
        Log::Writef("Cutscenes: %s: stream layer %s (unit %d) is off again before its node fires; turned on again",
                    g_playId.c_str(), name, t.unit);
        all = false;
    }
    for (TurnedOn& t : g_turnedOff) {
        if (LayerActive(t.unit, t.layer) != 1 || t.tries > 3) continue;   // (off, or not known)
        ++t.tries;
        TurnOffLayer(t.unit, t.layer);
        char name[kLayerNameSize];
        if (!LayerName(t.unit, t.layer, name, sizeof(name))) snprintf(name, sizeof(name), "layer %d", t.layer);
        Log::Writef("Cutscenes: %s: stream layer %s (unit %d) is on again before its node fires; turned off again",
                    g_playId.c_str(), name, t.unit);
        all = false;
    }
    if (!all) {
        g_layersAt = now;
        g_birthWaitSince = 0;
    }
    return all;
}

void Play(const std::string& id, int unitId) {
    const int index = CineIndex(unitId, id.c_str());
    if (index < 0) {
        g_phase = Phase::Idle;
        SetStatus(index == -1 ? "%s is not in its unit's cutscene list" : "%s: its unit's data is not there", id.c_str());
        return;
    }
    if (!StartCine(index, unitId)) {
        g_phase = Phase::Idle;
        SetStatus("%s (index %d in unit %d) did not start", id.c_str(), index, unitId);
        return;
    }
    g_phase = Phase::Playing;
    g_phaseSince = g_lastFrameLog = GetTickCount64();
    g_seenPlaying = false;
    SetStatus("%s started (index %d in %s, unit ID %d)", id.c_str(), index, g_playUnit.c_str(), unitId);
}
}  // namespace

void Initialize() {
    g_gameReady = GameBuild::VerifyBytes(kRvaCineGet, kCineGetSig, sizeof(kCineGetSig)) &&
                  GameBuild::VerifyBytes(kRvaCinePlay, kCinePlaySig, sizeof(kCinePlaySig)) &&
                  GameBuild::VerifyBytes(kRvaCineIsPlaying, kCineIsPlayingSig, sizeof(kCineIsPlayingSig)) &&
                  GameBuild::VerifyBytes(kRvaCineStop, kCineStopSig, sizeof(kCineStopSig)) &&
                  GameBuild::VerifyBytes(kRvaCineFrame, kCineFrameSig, sizeof(kCineFrameSig)) &&
                  GameBuild::VerifyBytes(kRvaGetLevelWithId, kGetLevelWithIdSig, sizeof(kGetLevelWithIdSig)) &&
                  GameBuild::VerifyBytes(kRvaHandlerIsPlaying, kHandlerIsPlayingSig, sizeof(kHandlerIsPlayingSig)) &&
                  GameBuild::VerifyBytes(kRvaHandlerName, kHandlerNameSig, sizeof(kHandlerNameSig)) &&
                  GameBuild::VerifyBytes(kRvaNetManagerGet, kNetManagerGetSig, sizeof(kNetManagerGetSig)) &&
                  GameBuild::VerifyBytes(kRvaReturnToMainMenu, kReturnToMainMenuSig, sizeof(kReturnToMainMenuSig));
    g_statesReady = g_gameReady && GameBuild::VerifyBytes(kRvaGetCurrGameState, kGetCurrGameStateSig, sizeof(kGetCurrGameStateSig)) &&
                    GameBuild::VerifyBytes(kRvaGetCurrNetState, kGetCurrNetStateSig, sizeof(kGetCurrNetStateSig));
    if (g_gameReady && !g_statesReady)
        Log::Write("Cutscenes: NetManager::GetCurrGameState not where expected; the way back to the viewer is not timed by the "
                   "game's states");
    if (g_gameReady) {
        const std::uintptr_t base = GameBuild::Base();
        g_cineGet = reinterpret_cast<CineGetFn>(base + kRvaCineGet);
        g_cinePlay = reinterpret_cast<CinePlayFn>(base + kRvaCinePlay);
        g_cineIsPlaying = reinterpret_cast<CineIsPlayingFn>(base + kRvaCineIsPlaying);
        g_cineStop = reinterpret_cast<CineStopFn>(base + kRvaCineStop);
        g_cineFrame = reinterpret_cast<CineFrameFn>(base + kRvaCineFrame);
        g_getLevel = reinterpret_cast<GetLevelFn>(base + kRvaGetLevelWithId);
        g_handlerIsPlaying = reinterpret_cast<HandlerIsPlayingFn>(base + kRvaHandlerIsPlaying);
        g_layersReady = GameBuild::VerifyBytes(kRvaStreamLayerManagerGet, kStreamLayerManagerGetSig, sizeof(kStreamLayerManagerGetSig)) &&
                        GameBuild::VerifyBytes(kRvaActivateStreamLayer, kActivateStreamLayerSig, sizeof(kActivateStreamLayerSig));
        if (!g_layersReady) Log::Write("Cutscenes: StreamLayerManager::ActivateStreamLayer does not match; cutscene layers are left as they are");
        g_layersOffReady = g_layersReady &&
                           GameBuild::VerifyBytes(kRvaDeactivateStreamLayer, kDeactivateStreamLayerSig, sizeof(kDeactivateStreamLayerSig)) &&
                           GameBuild::VerifyBytes(kRvaDeactivateStreamLayer + 0x1A, kDeactivateStreamLayerActionSig,
                                                  sizeof(kDeactivateStreamLayerActionSig));
        if (g_layersReady && !g_layersOffReady)
            Log::Write("Cutscenes: StreamLayerManager::DeactivateStreamLayer does not match; cutscenes' layersOff are left on");
        g_unitLoadReady = GameBuild::VerifyBytes(kRvaQueueUnitLoad, kQueueUnitLoadSig, sizeof(kQueueUnitLoadSig));
        if (!g_unitLoadReady) Log::Write("Cutscenes: STREAM_QueueUnitLoad does not match; cutscenes' units are not streamed in");
        // UnitStreamLayerNode: its vtable's ProcessMsg, whose input 0 calls StreamLayerManager::Get, reads +0x24 / +0x20
        // and calls ActivateStreamLayer (retail 0x74F957)
        static constexpr std::uint8_t kLayerNodeActivateSig[] = {0xE8, 0x54, 0x26, 0xCB, 0xFF, 0x44, 0x8B, 0x43, 0x24, 0x48,
                                                                 0x8B, 0xC8, 0x8B, 0x53, 0x20, 0xE8, 0x85, 0x58, 0xCA, 0xFF};
        g_storyLayersReady = g_layersReady &&
                             *reinterpret_cast<const std::uintptr_t*>(base + kRvaUnitStreamLayerNodeVtable + 8) ==
                                 base + kRvaUnitStreamLayerNodeProcess &&
                             GameBuild::VerifyBytes(kRvaUnitStreamLayerNodeProcess + 0x17, kLayerNodeActivateSig,
                                                    sizeof(kLayerNodeActivateSig));
        g_layerActiveReady = GameBuild::VerifyBytes(kRvaIsStreamLayerActive, kIsStreamLayerActiveSig, sizeof(kIsStreamLayerActiveSig));
    } else {
        Log::Write("Cutscenes: the game's cinematic functions are not where this framework expects them; playing is off");
    }
    g_scanState = 1;
    g_scanStatus = "scanning the game's archives";
    std::thread(ScanThread).detach();
}

namespace {
// ---- the game's stream layer and time-of-day switches, logged ----
// What the story switches on the way to a scene, and what switches a layer the viewer turned on (0.9.372: Whispers in
// the Dark's Cine_Whispers_fx, turned on before its node fired, was off when it played; its lighting and snow were not
// the story's). Every stream layer action goes through StreamLayerManager::SetStreamLayerSafely(manager, the unit's
// PlacementHashKey, layer, action) (retail 0x428B10, TR2 0x454600; ActivateStreamLayer calls it with action 2, and the
// action graphs' layer nodes go through ActivateStreamLayer / its siblings); cdc::StreamLayerAction: 0 none, 1 load,
// 2 activate, 3 deactivate, 4 hide, 5 load and hide. The unit's key: GenerateHashKeyForUnitObject (retail 0x234110),
// an FNV-1a style hash of the unit's ID (UnitHashKey). The time of day: WeatherManager::ActivateTimeOfDayType(manager,
// type) (retail 0x4859F0, TR2 0x4B5D70): sets m_activeTimeOfDayType (+0x10; 0xFF none), then tells the action graphs'
// time-of-day event nodes (NotifyAGNodesOfTimeOfDayEvent) and EnvironmentMaterialOverrideManager (the snow on surfaces:
// ProcessMsgTimeOfDayChange); its types are GlobalWeather's (retail 0x2D588B8: +0x10 the list, 8 bytes each, its debug
// name first; +0x18 the count). Logged outside the game's loading, with the code that asked (ROTTR.exe+...), 300 lines
// a minute at most; an activate of a layer already on and a deactivate of one already off are left out.
constexpr std::uintptr_t kRvaSetStreamLayerSafely = 0x00428B10ull, kRvaActivateTimeOfDayType = 0x004859F0ull,
                         kRvaWeatherManagerPtr = 0x02D58860ull, kRvaGlobalWeatherPtr = 0x02D588B8ull;
constexpr std::uint8_t kSetStreamLayerSafelySig[] = {0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x74, 0x24,
                                                     0x18, 0x48, 0x89, 0x54, 0x24, 0x10, 0x57, 0x48, 0x83,
                                                     0xEC, 0x20, 0x48, 0x8B, 0xF1, 0x41, 0x0F, 0xB6, 0xD9};
constexpr std::uint8_t kActivateTimeOfDayTypeSig[] = {0x48, 0x89, 0x5C, 0x24, 0x08, 0x57, 0x48, 0x83, 0xEC, 0x20, 0x8B, 0xFA,
                                                      0x48, 0x8B, 0xD9, 0x81, 0xFA, 0xFF, 0x00, 0x00, 0x00, 0x74, 0x0D, 0x0F};
using SetStreamLayerSafelyFn = void(__fastcall*)(void*, std::uint64_t, int, std::uint8_t);
using ActivateTimeOfDayTypeFn = void(__fastcall*)(void*, unsigned);
SetStreamLayerSafelyFn g_origSetStreamLayerSafely = nullptr;
ActivateTimeOfDayTypeFn g_origActivateTimeOfDayType = nullptr;
std::atomic<bool> g_switchLogReady{false};
std::mutex g_switchLogMutex;
std::unordered_map<std::uint64_t, int> g_unitOfKey;   // (found ones)
ULONGLONG g_switchWindow = 0;
int g_switchLines = 0, g_switchSkipped = 0;

std::uint64_t UnitHashKey(std::uint32_t unitId) {
    constexpr std::uint64_t kBasis = 0xCBF29CE484222325ull, kPrime = 0x100000001B3ull;
    std::uint64_t h = unitId ? std::uint64_t(unitId) : kBasis;
    h *= 0x9FFAAC085635BC91ull;
    if (!h) h = kBasis;
    for (int i = 0; i < 4; ++i) {
        h *= kPrime;
        h ^= 0xFF;
    }
    return h;
}
int UnitOfKey(std::uint64_t key) {   // (under g_switchLogMutex)
    if (const auto it = g_unitOfKey.find(key); it != g_unitOfKey.end()) return it->second;
    int unit = -1;
    for (std::uint32_t id = 1; id < 0x10000u && unit < 0; ++id)
        if (UnitHashKey(id) == key) unit = int(id);
    g_unitOfKey[key] = unit;
    return unit;
}
bool SwitchLogLine() {   // (under g_switchLogMutex)
    const ULONGLONG now = GetTickCount64();
    if (now - g_switchWindow >= 60000) {
        if (g_switchSkipped)
            Log::Writef("Cutscenes: %d more layer / time-of-day switches in that minute not logged", g_switchSkipped);
        g_switchWindow = now;
        g_switchLines = g_switchSkipped = 0;
    }
    if (g_switchLines >= 300) {
        ++g_switchSkipped;
        return false;
    }
    ++g_switchLines;
    return true;
}
std::string CodeAt(void* address) {
    const std::uintptr_t a = reinterpret_cast<std::uintptr_t>(address), base = GameBuild::Base();
    char text[48];
    if (a >= base && a - base < 0x4000000u) snprintf(text, sizeof(text), "ROTTR.exe+0x%llX", static_cast<unsigned long long>(a - base));
    else snprintf(text, sizeof(text), "0x%llX", static_cast<unsigned long long>(a));
    return text;
}
// A time-of-day type's debug name (GlobalWeather's list), or "". SEH only.
void TimeOfDayName(unsigned type, char* out, std::size_t capacity) noexcept {
    out[0] = 0;
    __try {
        auto* global = *reinterpret_cast<std::uint8_t**>(GameBuild::Base() + kRvaGlobalWeatherPtr);
        if (!global || type >= *reinterpret_cast<std::uint32_t*>(global + 0x18)) return;
        auto* list = *reinterpret_cast<std::uint8_t**>(global + 0x10);
        const char* name = list ? *reinterpret_cast<const char**>(list + std::size_t(type) * 8) : nullptr;
        std::size_t k = 0;
        for (; name && k + 1 < capacity && name[k]; ++k) out[k] = name[k];
        out[k] = 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        out[0] = 0;
    }
}
int ActiveTimeOfDay() noexcept {   // (0xFF none, -1 not known)
    __try {
        auto* manager = *reinterpret_cast<std::uint8_t**>(GameBuild::Base() + kRvaWeatherManagerPtr);
        return manager ? manager[0x10] : -1;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
}
std::string TimeOfDayText(int type) {
    if (type < 0) return "not known";
    if (type == 0xFF) return "none";
    char name[64];
    TimeOfDayName(unsigned(type), name, sizeof(name));
    return std::to_string(type) + (name[0] ? std::string(" (") + name + ")" : std::string());
}

void __fastcall HookSetStreamLayerSafely(void* manager, std::uint64_t key, int layer, std::uint8_t action) {
    if (g_switchLogReady.load(std::memory_order_relaxed) && action >= 2 && action <= 5 &&
        !Features::LoadingProfilerSessionActive()) {
        std::lock_guard lock(g_switchLogMutex);
        const int unit = UnitOfKey(key);
        const int was = unit > 0 ? LayerActive(unit, layer) : -1;
        if (!((action == 2 && was == 1) || (action == 3 && was == 0)) && SwitchLogLine()) {
            static const char* const kActions[] = {"none", "load", "on", "off", "hidden", "loaded hidden"};
            char name[kLayerNameSize];
            if (unit <= 0 || !LayerName(unit, layer, name, sizeof(name))) snprintf(name, sizeof(name), "layer %d", layer);
            const char* unitName = unit > 0 ? ZoneList::Name(std::uint16_t(unit)) : nullptr;
            Log::Detailf("Cutscenes: stream layer %s of %s (unit %d) %s%s, from %s%s", name, unitName ? unitName : "?", unit,
                        kActions[action], was == 1 ? " (was on)" : (was == 0 ? " (was off)" : ""),
                        CodeAt(_ReturnAddress()).c_str(), g_viewerLayerCall ? " (the viewer)" : "");
        }
    }
    g_origSetStreamLayerSafely(manager, key, layer, action);
}

void __fastcall HookActivateTimeOfDayType(void* manager, unsigned type) {
    if (g_switchLogReady.load(std::memory_order_relaxed) && !Features::LoadingProfilerSessionActive()) {
        std::lock_guard lock(g_switchLogMutex);
        const int was = ActiveTimeOfDay();
        if (was != int(type) && SwitchLogLine())
            Log::Detailf("Cutscenes: time of day %s made active (was %s), from %s", TimeOfDayText(int(type)).c_str(),
                        TimeOfDayText(was).c_str(), CodeAt(_ReturnAddress()).c_str());
    }
    g_origActivateTimeOfDayType(manager, type);
}

void CreateSwitchLogHooks() {
    const std::uintptr_t base = GameBuild::Base();
    if (!GameBuild::VerifyBytes(kRvaSetStreamLayerSafely, kSetStreamLayerSafelySig, sizeof(kSetStreamLayerSafelySig)) ||
        !GameBuild::VerifyBytes(kRvaActivateTimeOfDayType, kActivateTimeOfDayTypeSig, sizeof(kActivateTimeOfDayTypeSig))) {
        Log::Write("Cutscenes: SetStreamLayerSafely / ActivateTimeOfDayType not where expected; layer switches not logged");
        return;
    }
    if (MH_CreateHook(reinterpret_cast<void*>(base + kRvaSetStreamLayerSafely), reinterpret_cast<void*>(&HookSetStreamLayerSafely),
                      reinterpret_cast<void**>(&g_origSetStreamLayerSafely)) != MH_OK) {
        Log::Write("Cutscenes: the stream layer switch hook was not created; layer switches not logged");
        return;
    }
    if (MH_CreateHook(reinterpret_cast<void*>(base + kRvaActivateTimeOfDayType),
                      reinterpret_cast<void*>(&HookActivateTimeOfDayType),
                      reinterpret_cast<void**>(&g_origActivateTimeOfDayType)) != MH_OK) {
        MH_RemoveHook(reinterpret_cast<void*>(base + kRvaSetStreamLayerSafely));
        Log::Write("Cutscenes: the time-of-day hook was not created; layer switches not logged");
        return;
    }
    g_switchLogReady = true;
}
}  // namespace

// (v0.9.520) The time of day's own stream layers, switched with it when the Weather tab changes it (no checkpoint).
// Each unit has layers per time of day, TOD_<name> and VFX_<name> (its lighting and effects), and story ones
// (TOD_<name>_<stage>_Story, VFX_<name>_<stage>_Story: cutscenes\layers\*.txt lists them). A checkpoint's reload
// turned the old one's off (0.9.519's log, Dawn to Dusk in vh_valley_hub: TOD_Dawn_VH3_Story and VFX_Dawn) and the
// level's load gave the new one's; a change without the reload left them as they were.
// (v0.9.521) 0.9.520 mapped only the layers on at each change, so a story layer with no counterpart (vh_valley_hub's
// TOD_Dawn_VH3_Story: no Dusk, Night or Day one) went off at the first change and never came back: back at Dawn the
// valley had VFX_Dawn without its Dawn lighting (the user: after a few changes it looked like a nightmare section).
// Now each unit keeps what the game itself gave it: the first look at a unit after each load (the loading session's
// number) takes its time-of-day layers on then as the game's for the time of day they are named for. A change to a
// time of day the game gave the unit is that set again; to another, each layer the game gave is mapped to the new
// time of day's name where the unit has that layer (a story layer without one stays off, as a reload leaves it).
// Every time-of-day layer of the unit (TOD_ or VFX_ with a time of day's name among its parts) is turned on or off
// to match. Kept across loads and visits while it agrees with what the game gives later (a story stage since: the
// layers it gives differ where both times of day have them; then forgotten and taken anew). Nothing while the game
// loads (0.9.520 looked at units being loaded: access violations caught in the game's LayerActive, an 83 ms hitch); a
// change the Weather tab made during a load is matched 2 s after it (TimeOfDayLayersDue), when the game's own
// switches are done.
namespace {
struct UnitTimeOfDayLayers {
    std::map<std::string, std::vector<std::string>> natural;   // by time of day (lower case): the layers the game gave
    std::uint64_t session = ~0ull;                              // the loading session it was last looked at in
};
std::map<std::string, UnitTimeOfDayLayers> g_unitTimeOfDay;   // (game thread) by unit name
std::uint64_t g_todDoneSession = ~0ull;                       // (game thread) TimeOfDayLayersDue
std::uint64_t g_todSettleSession = ~0ull;
ULONGLONG g_todSettleSince = 0;

std::string TodLower(std::string text) {
    for (char& c : text) c = char(std::tolower(static_cast<unsigned char>(c)));
    return text;
}
bool TodSame(const std::string& a, const std::string& b) { return _stricmp(a.c_str(), b.c_str()) == 0; }
bool TodHas(const std::vector<std::string>& list, const std::string& name) {
    for (const auto& item : list)
        if (TodSame(item, name)) return true;
    return false;
}
std::vector<std::string> TodSplit(const std::string& name) {
    std::vector<std::string> parts;
    std::size_t at = 0;
    for (;;) {
        const std::size_t next = name.find('_', at);
        parts.push_back(name.substr(at, next == std::string::npos ? std::string::npos : next - at));
        if (next == std::string::npos) break;
        at = next + 1;
    }
    return parts;
}
std::string TodJoin(const std::vector<std::string>& parts) {
    std::string name = parts.empty() ? std::string() : parts[0];
    for (std::size_t k = 1; k < parts.size(); ++k) name += "_" + parts[k];
    return name;
}
std::string TodList(const std::vector<std::string>& list) {
    std::string text;
    for (const auto& item : list) text += (text.empty() ? "" : ", ") + item;
    return text.empty() ? std::string("none") : text;
}
}  // namespace

bool TimeOfDayLayersDue() {
    if (Features::LoadingProfilerSessionActive()) {
        g_todSettleSince = 0;
        return false;
    }
    const std::uint64_t session = Features::LoadingSessionId();
    if (session == g_todDoneSession) return false;
    const ULONGLONG now = GetTickCount64();
    if (!g_todSettleSince || g_todSettleSession != session) {
        g_todSettleSince = now;
        g_todSettleSession = session;
        return false;
    }
    if (now - g_todSettleSince < 2000) return false;
    g_todDoneSession = session;
    return true;
}

void SwitchTimeOfDayLayers(int toType, const char* reason) {
    if (!g_layersReady || !g_layersOffReady || !g_layerActiveReady) return;
    if (toType < 0 || toType == 0xFF || Features::LoadingProfilerSessionActive()) return;
    char to[64];
    TimeOfDayName(unsigned(toType), to, sizeof(to));
    if (!to[0]) return;
    std::vector<std::string> todNames;   // every time of day's name (GlobalWeather's list)
    for (unsigned t = 0; t < 16; ++t) {
        char name[64];
        TimeOfDayName(t, name, sizeof(name));
        if (!name[0]) break;
        todNames.push_back(name);
    }
    // the part of a layer's name that is a time of day's (its index), else -1; only names starting TOD_ or VFX_
    auto todPart = [&](const std::vector<std::string>& parts) -> int {
        if (parts.size() < 2 || !(TodSame(parts[0], "TOD") || TodSame(parts[0], "VFX"))) return -1;
        for (std::size_t j = 1; j < parts.size(); ++j)
            for (const std::string& t : todNames)
                if (TodSame(parts[j], t)) return int(j);
        return -1;
    };
    const std::uint64_t session = Features::LoadingSessionId();
    LoadedUnit units[32];
    const int unitCount = LoadedUnits(units, 32);
    int withLayers = 0, changed = 0;
    for (int u = 0; u < unitCount; ++u) {
        const int count = UnitLayerNames(units[u].id);
        if (count <= 0) continue;
        std::vector<std::string> names(static_cast<std::size_t>(count));
        for (int i = 0; i < count; ++i) names[std::size_t(i)] = g_layerNames[i];
        std::vector<int> tagged;        // the unit's time-of-day layers
        std::vector<std::string> now;   // those on
        for (int i = 0; i < count; ++i) {
            if (todPart(TodSplit(names[std::size_t(i)])) < 0) continue;
            tagged.push_back(i);
            if (LayerActive(units[u].id, i) == 1) now.push_back(names[std::size_t(i)]);
        }
        if (tagged.empty()) continue;
        ++withLayers;
        // layers to a time of day: each with its name in the time of day's place, where the unit has that layer
        auto mapTo = [&](const std::vector<std::string>& layers, const std::string& tod, std::vector<std::string>* missing) {
            std::vector<std::string> out;
            for (const std::string& layer : layers) {
                std::vector<std::string> parts = TodSplit(layer);
                const int j = todPart(parts);
                if (j < 0) continue;
                parts[std::size_t(j)] = tod;
                const std::string want = TodJoin(parts);
                bool there = false;
                for (const int i : tagged)
                    if (TodSame(names[std::size_t(i)], want)) {
                        if (!TodHas(out, names[std::size_t(i)])) out.push_back(names[std::size_t(i)]);
                        there = true;
                        break;
                    }
                if (!there && missing && !TodHas(*missing, want)) missing->push_back(want);
            }
            return out;
        };
        UnitTimeOfDayLayers& unit = g_unitTimeOfDay[units[u].name];
        if (unit.session != session) {
            if (now.empty()) continue;   // (none on: the level's not there yet)
            // the time of day they are for: the one most of them are named for
            std::map<std::string, int> votes;
            std::string at;
            for (const std::string& layer : now) {
                const std::vector<std::string> parts = TodSplit(layer);
                const std::string tod = parts[std::size_t(todPart(parts))];
                const int n = ++votes[TodLower(tod)];
                if (at.empty() || n > votes[TodLower(at)]) at = tod;
            }
            // what was kept from before, against it: where both times of day have a layer, the game gave both or neither
            bool agrees = true;
            for (const auto& [tod, layers] : unit.natural) {
                for (const std::string& l : mapTo(layers, at, nullptr)) agrees = agrees && TodHas(now, l);
                for (const std::string& l : now) {
                    const std::vector<std::string> back = mapTo({l}, tod, nullptr);
                    agrees = agrees && (back.empty() || TodHas(layers, back[0]));
                }
            }
            if (!agrees) {
                Log::Writef("Weather: %s's time-of-day layers are not as the game gave them before (the story moved on?): "
                            "those kept are forgotten", units[u].name);
                unit.natural.clear();
            }
            unit.natural[TodLower(at)] = now;
            unit.session = session;
            Log::Writef("Weather: %s's time-of-day layers as the game gave them (%s): %s", units[u].name, at.c_str(),
                        TodList(now).c_str());
        }
        // the target: the game's own for that time of day, else the game's mapped to it
        std::vector<std::string> target, missing;
        std::string from;
        const auto own = unit.natural.find(TodLower(to));
        if (own != unit.natural.end()) {
            target = own->second;
            from = "as the game gave them";
        } else {
            for (const auto& [tod, layers] : unit.natural) {
                for (const std::string& l : mapTo(layers, to, &missing))
                    if (!TodHas(target, l)) target.push_back(l);
                from += (from.empty() ? "from the game's at " : ", ") + tod;
            }
        }
        std::vector<std::string> on, off;
        g_viewerLayerCall.store(true);
        for (const int i : tagged) {
            const std::string& name = names[std::size_t(i)];
            const bool want = TodHas(target, name);
            const int active = LayerActive(units[u].id, i);
            if (want && active == 0) {
                ActivateLayer(units[u].id, i);
                on.push_back(name);
            } else if (!want && active == 1) {
                DeactivateLayer(units[u].id, i);
                off.push_back(name);
            }
        }
        g_viewerLayerCall.store(false);
        if (on.empty() && off.empty()) continue;
        ++changed;
        std::string none;
        for (const std::string& m : missing)
            if (!TodHas(target, m)) none += (none.empty() ? "; the unit has no " : ", ") + m;
        Log::Writef("Weather: %s's time-of-day layers to %s (%s, %s): on %s; off %s%s", units[u].name, to, from.c_str(),
                    reason, TodList(on).c_str(), TodList(off).c_str(), none.c_str());
    }
    if (!changed)
        Log::Writef("Weather: the time-of-day layers of the loaded units (%d, %d with any) already as for %s (%s)", unitCount,
                    withLayers, to, reason);
}

bool CreateHooks() {
    if (!GameBuild::VerifyBytes(kRvaCineNodeCtor, kCineNodeCtorSig, sizeof(kCineNodeCtorSig)) ||
        !GameBuild::VerifyBytes(kRvaCineNodeDtor, kCineNodeDtorSig, sizeof(kCineNodeDtorSig)) ||
        !GameBuild::VerifyBytes(kRvaCineNodeProcess, kCineNodeProcessSig, sizeof(kCineNodeProcessSig)) ||
        *reinterpret_cast<const std::uintptr_t*>(GameBuild::Base() + kRvaCineNodeVtable + 8) !=
            GameBuild::Base() + kRvaCineNodeProcess) {
        Log::Write("Cutscenes: Cine_PlayCinematic is not where this framework expects it; cutscenes after a checkpoint won't start");
        return false;
    }
    const std::uintptr_t base = GameBuild::Base();
    if (MH_CreateHook(reinterpret_cast<void*>(base + kRvaCineNodeCtor), reinterpret_cast<void*>(&HookCineNodeCtor),
                      reinterpret_cast<void**>(&g_origCineNodeCtor)) != MH_OK)
        return false;
    if (MH_CreateHook(reinterpret_cast<void*>(base + kRvaCineNodeDtor), reinterpret_cast<void*>(&HookCineNodeDtor),
                      reinterpret_cast<void**>(&g_origCineNodeDtor)) != MH_OK) {
        MH_RemoveHook(reinterpret_cast<void*>(base + kRvaCineNodeCtor));
        return false;
    }
    g_nodeHooks = true;
    CreateSwitchLogHooks();
    g_agReady = GameBuild::VerifyBytes(kRvaAgAddMessage, kAgAddMessageSig, sizeof(kAgAddMessageSig)) &&
                GameBuild::VerifyBytes(kRvaAllPlacementsBirthed, kAllPlacementsBirthedSig, sizeof(kAllPlacementsBirthedSig));
    if (!g_agReady) Log::Write("Cutscenes: ActionGraphInstance::AddMessage not where expected; cutscene nodes are fired alone");
    {   // the action graph manager and its relay lists where ActionGraphInstanceOutputsNode::ProcessMsg reads them
        const auto* ref = reinterpret_cast<const std::uint8_t*>(base + kRvaAgManagerRef);
        const auto* call = reinterpret_cast<const std::uint8_t*>(base + kRvaAgRelaysCall);
        g_storyEventReady =
            g_agReady && ref[0] == 0x48 && ref[1] == 0x8B && ref[2] == 0x0D &&
            kRvaAgManagerRef + 7 + std::uintptr_t(std::intptr_t(*reinterpret_cast<const std::int32_t*>(ref + 3))) == kRvaAgManagerPtr &&
            call[0] == 0xE8 &&
            kRvaAgRelaysCall + 5 + std::uintptr_t(std::intptr_t(*reinterpret_cast<const std::int32_t*>(call + 1))) == kRvaAgRelaysOf;
        if (!g_storyEventReady) Log::Write("Cutscenes: the action graph manager is not where expected; no story events are sent");
        // AGTimeDelayNode's ProcessMsg (its vtable +8) and ActionGraphNode::OutputData (the relay's forward jumps there)
        const auto* forward = reinterpret_cast<const std::uint8_t*>(
            *reinterpret_cast<const std::uintptr_t*>(base + kRvaAgRelayNodeVtable + 0x28));
        g_storyDelaysReady =
            g_storyEventReady &&
            GameBuild::VerifyBytes(kRvaTimeDelayProcessMsg, kTimeDelayProcessMsgSig, sizeof(kTimeDelayProcessMsgSig)) &&
            GameBuild::VerifyBytes(kRvaAgOutputData, kAgOutputDataSig, sizeof(kAgOutputDataSig)) &&
            *reinterpret_cast<const std::uintptr_t*>(base + kRvaTimeDelayNodeVtable + 8) == base + kRvaTimeDelayProcessMsg &&
            forward[0] == 0xE9 &&
            reinterpret_cast<std::uintptr_t>(forward) + 5 + std::uintptr_t(std::intptr_t(*reinterpret_cast<const std::int32_t*>(forward + 1))) ==
                base + kRvaAgOutputData;
        if (g_storyEventReady && !g_storyDelaysReady)
            Log::Write("Cutscenes: AGTimeDelayNode::ProcessMsg or ActionGraphNode::OutputData not where expected; story events wait "
                       "for nothing and keep the story's delays");
    }
    return true;
}

std::string Norm(const std::string& text) {
    std::string out;
    for (const char c : text)
        if (std::isalnum(static_cast<unsigned char>(c))) out.push_back(char(std::tolower(static_cast<unsigned char>(c))));
    return out;
}

// Each cutscene without a start in cutscenes.json gets a known start (kKnownStarts, first since 0.9.389: a PCAP entry
// can be after the scene, as "PCAP: 46 The Deathless Ones"), else the Chapter Select entry named after it, if there is
// one: "PCAP: 23 A Connection" (its scene number), "Cinematic: Rock Bottom (cine_01_Rock_Bottom)" (its title).
int MapStarts(const std::vector<Features::UnitInfo>& units) {
    std::lock_guard lock(g_catalogMutex);
    int mapped = 0;
    // Starts the game's list does not name: a story checkpoint before the scene, in the unit that lists it, chosen from
    // the checkpoints' names and order (not seen in game yet, except Execution's and The Reckoning's). The viewer fires
    // the scene's node there; when that checkpoint has none, it starts the scene directly, its layer on.
    // Execution plays in cine_execution, a unit the story loads after The Reckoning (pt_prophet_tomb; the game's voice
    // files: 350_ec_ending_cinematics\350_ec_010_57_reckoning, then 350_ec_020_57a_execution), A New Beginning and the
    // end credits: from "Chamber of Souls: Konstantin - Complete", by way of The Reckoning (0.9.361: The Reckoning,
    // skipped, led on to A New Beginning, the credits, then Execution).
    struct KnownStart {
        const char* id;
        const char* runnable;
        const char* via;
    };
    static const KnownStart kKnownStarts[] = {
        {"57b_execution_cine", "Pt_Prophet_Tomb\\pt_098_konstantin_complete", "57_thereckoning_cine"},
        {"57_thereckoning_cine", "Pt_Prophet_Tomb\\pt_098_konstantin_complete", nullptr},
        {"59_anewbeginning_cine", "Pt_Prophet_Tomb\\pt_098_konstantin_complete", "57_thereckoning_cine"},
        // the opening (the game plays it while "Cinematic: Long Journey" loads; 13_longjourney_cine is cut)
        {"14_expedtion_a_cine", "OMS_Avalanche\\AV_000_Walk_Talk\\av_000_01_Long_Journey", nullptr},
        // the Syria car chase: "Cinematic: Car Chase" (bp_010_cliffside's level start; its next entry is PostCines)
        {"03_carcrash_cine_03_part01", "black_sea_prologue\\bp_010_cliffside\\bp_010_cliffside_000_LevelStart", nullptr},
        // av_001_ice_climb's only story checkpoint before Separation (PCAP: 19 is at av_001_035_Overhang_Reach)
        {"15_almostthere_cine", "OMS_Avalanche\\AV_001_Ice_Climb\\av_001_02_Ice_Climb_Start", nullptr},
        // Gulag Escape: after Ana (000) and Escape Artist (010), the checkpoint out of the cell (020, Twisted Past)
        {"27b_trustissues_cine", "Gulag_Escape\\Ge_010_ext_camp\\ge_010_ext_camp_020_CellEscaped", nullptr},
        // the Geothermal Valley: its first checkpoint (Campsite), then To the Tower's own
        {"36_meettheremnant_cine", "Vh_Valley_Hub\\vh1_100_level_start", nullptr},
        {"40_tothetower_cine", "Vh_Valley_Hub\\vh1_180_to_the_tower_start", nullptr},
        // the Byzantine Ruins: Into the Ruins, then Courtyard Infiltration for the scenes after the incursion
        {"40b_courtyardincursion_cine", "Remnant_Ruins\\br_010_outer_city\\br_010_outer_city_100_LevelStart", nullptr},
        {"40c_remnantresistance_cine", "Remnant_Ruins\\br_010_outer_city\\br_010_outer_city_110_IncursionStart", nullptr},
        {"40d_planofaction_cine", "Remnant_Ruins\\br_010_outer_city\\br_010_outer_city_110_IncursionStart", nullptr},
        // back in the valley after the Flooded Archive: VH3's start (Upper Ridge); A Choice comes with it (KnownChains)
        {"47c_revealingthepath_cine", "Vh_Valley_Hub\\vh3_300_level_start", nullptr},
        // gr_connector_to_sh at the cliff (Safe Landing - Cliff): from its first checkpoint (Safe Landing - Start, up to
        // 0.9.368) the scene played in white, nothing but its actors and the scope's tripod: that checkpoint is in the
        // cave, with VFX_Connector_Only on and the cliff's area not there
        {"48a_locatingthesource_cine", "Gr_gulag_rescue\\Gr_connector_to_sh\\gr_020_connector_cliff", nullptr},
        // "VH5: Aftermath"
        {"60_aftermath_cine", "Vh_Valley_Hub\\vh5_420_Aftermath", nullptr},
        // 0.9.389 (not seen in game yet, but The Deathless Ones): Gulag Escape's checkpoint before "Control Room - Start"
        // (ge_010_ext_camp_060_AtlasCinematicEnd: after the scene, nothing played there in 0.9.388's log)
        {"28_atlas_cine", "Gulag_Escape\\Ge_010_ext_camp\\ge_010_ext_camp_040_TheYardEnd", nullptr},
        // the end of the Ice Tomb's run out ("Run out through the door"; the prayer room's checkpoint after it is
        // it_040_prayer_room_000_OutThroughTheInDoor), played from "Sauna Fight" (it_030_tomb_035_EnteringChurch) with
        // the layers the story has at the scene's start (0.9.391's log of the story: KnownLayers, KnownLayersOff,
        // KnownLayersOffAtStart). From Sauna Fight alone (0.9.389) pieces of the level were missing (the user); from
        // "Run Out" itself (it_030_tomb_090_RunOutStart, 0.9.390) they were there, but that checkpoint starts the
        // collapse: ice_cave_collapse_intro played at the load (skipped), the screen shook with a rumble all through
        // the scene (fxt_puzzle_tremors at Lara) and, 5 s in, the collapse's death (animhost_iss_wall_of_doom_rock_
        // death) was spawned at Lara: its rocks covered her (the user)
        {"35_inthroughtheoutdoor_cine", "ice_tomb\\it_030_tomb\\it_030_tomb_035_EnteringChurch", nullptr},
        // after Kill or Be Killed, before To the Tower (vh1_180)
        {"39b_setback_cine", "Vh_Valley_Hub\\vh1_175_kill_or_be_killed_combat_complete", nullptr},
        // "Flooded Archive: Atlas Vista - Pre Reveal": the story plays it there after the load (0.9.388's log, in
        // fa_020_archive with its cine_deathless layer); "PCAP: 46 The Deathless Ones" (fa_020_archive_000_LevelStart)
        // is after it: nothing played there
        {"46_thedeathlessones_cine", "Flooded_Archive\\fa_010_church\\fa_010_church_040_PostScene45", nullptr},
        // VH3's start, as Revealing the Path (Reunion's props layer is off there; KnownLayers)
        {"47b_reunion_cine", "Vh_Valley_Hub\\vh3_300_level_start", nullptr},
    };
    for (const KnownStart& known : kKnownStarts)
        for (Entry& e : g_catalog) {
            if (_stricmp(e.id.c_str(), known.id) != 0 || !e.startUnit.empty()) continue;
            for (const auto& u : units) {
                const char* unit = ZoneList::Name(u.unitId);
                if (!unit || _stricmp(u.runnableName, known.runnable) != 0) continue;
                e.startUnit = unit;
                e.startRunnable = u.runnableName;
                e.via = known.via ? known.via : "";
                ++mapped;
                Log::Writef("Cutscenes: %s starts from Chapter Select \"%s\" (%s)%s%s", e.id.c_str(), u.storyName, unit,
                            e.via.empty() ? "" : " by way of ", e.via.c_str());
                break;
            }
        }
    for (const auto& u : units) {
        const std::string story = u.storyName;
        std::string rest;
        if (const auto at = story.find("PCAP:"); at != std::string::npos) rest = story.substr(at + 5);
        else if (const auto at2 = story.find("Cinematic:"); at2 != std::string::npos) rest = story.substr(at2 + 10);
        else continue;
        if (const auto paren = rest.find(" ("); paren != std::string::npos) rest.resize(paren);
        while (!rest.empty() && rest.front() == ' ') rest.erase(rest.begin());
        std::string token, title = rest;
        if (!rest.empty() && std::isdigit(static_cast<unsigned char>(rest[0]))) {
            const auto space = rest.find(' ');
            token = Lower(rest.substr(0, space));
            title = space == std::string::npos ? std::string() : rest.substr(space + 1);
            if (const auto slash = title.find(" / "); slash != std::string::npos) title.resize(slash);
        }
        const char* unit = ZoneList::Name(u.unitId);
        if (!unit) continue;
        const std::string wanted = Norm(title);
        for (Entry& e : g_catalog) {
            if (e.video || !e.startUnit.empty() || CutFromGame(e.id)) continue;
            const std::string id = Lower(e.id);
            const bool byNumber = !token.empty() && id.rfind(token + "_", 0) == 0;
            const bool byTitle = !wanted.empty() && (Norm(e.title) == wanted || (wanted.size() >= 6 && Norm(id).find(wanted) != std::string::npos));
            if (!byNumber && !byTitle) continue;
            e.startUnit = unit;
            e.startRunnable = u.runnableName;
            ++mapped;
            Log::Writef("Cutscenes: %s starts from Chapter Select \"%s\" (%s)", e.id.c_str(), u.storyName, unit);
            break;
        }
    }
    if (mapped) WriteCatalog(g_catalog);
    return mapped;
}

// The game's Chapter Select entries, once, to cutscenes\chapter_select.txt (the story cutscenes are started by their
// entries' runnables: "Cinematic: Rock Bottom (cine_01_Rock_Bottom)" / black_sea_prologue\cine_01_Rock_Bottom).
void WriteChapterSelectList() {
    static bool written = false;
    static std::uint64_t seenVersion = 0;
    static ULONGLONG stableSince = 0;
    if (written) return;
    const std::uint64_t version = Features::UnitListVersion();
    const int count = Features::UnitCount();
    const ULONGLONG now = GetTickCount64();
    if (!version || count <= 0) return;
    if (version != seenVersion) {
        seenVersion = version;
        stableSince = now;
        return;
    }
    if (now - stableSince < 5000 || g_scanState.load() == 1) return;
    written = true;
    std::vector<Features::UnitInfo> units(std::size_t(count) + 16);
    units.resize(std::size_t(std::max(0, Features::CopyUnits(units.data(), int(units.size())))));
    std::error_code ec;
    fs::create_directories(CatalogPath().parent_path(), ec);
    std::ofstream out(CatalogPath().parent_path() / L"chapter_select.txt", std::ios::binary | std::ios::trunc);
    out << "release\tsprint\tstory\tunit\trunnable\n";
    int cinematics = 0;
    for (const auto& u : units) {
        const char* unit = ZoneList::Name(u.unitId);
        out << u.releaseName << '\t' << u.sprintName << '\t' << u.storyName << '\t' << (unit ? unit : "?") << '\t'
            << u.runnableName << '\n';
        cinematics += _strnicmp(u.storyName, "Cinematic", 9) == 0;
    }
    Log::Writef("Cutscenes: %zu Chapter Select entries (%d named Cinematic) written to cutscenes\\chapter_select.txt",
                units.size(), cinematics);
    if (g_scanState.load() == 2) Log::Writef("Cutscenes: %d cutscenes matched to Chapter Select entries", MapStarts(units));
}

// The cutscene's "hide" objects hidden and its "remove" objects removed, every second from the load's end to the
// cutscene's end.
ULONGLONG g_hideAt = 0;
bool g_hideLogged = false, g_removeLogged = false;
void HideForCutscene(ULONGLONG now) {
    if (now < g_hideAt) return;
    g_hideAt = now + 1000;
    std::vector<std::string> names, removeNames;
    {
        std::lock_guard lock(g_catalogMutex);
        for (const Entry& e : g_catalog)
            if (e.id == g_playId) {
                names = e.hide;
                removeNames = e.remove;
            }
    }
    if (!removeNames.empty()) {
        int found = 0;
        const int removed = Features::RemoveInstancesOfObjects(removeNames, &found);
        if (removed > 0 || (!g_removeLogged && found <= 0 && now - g_phaseSince > 5000)) {
            g_removeLogged = true;
            if (found < 0)
                Log::Writef("Cutscenes: %s: its remove list names no object of the game's object list", g_playId.c_str());
            else
                Log::Writef("Cutscenes: %s: %d instance%s of its remove list removed (%d live)", g_playId.c_str(), removed,
                            removed == 1 ? "" : "s", found);
        }
    }
    if (names.empty()) return;
    int found = 0;
    const int hidden = Features::HideInstancesOfObjects(names, &found);
    if (hidden > 0 || (!g_hideLogged && found <= 0 && now - g_phaseSince > 5000)) {
        g_hideLogged = true;
        if (found < 0)
            Log::Writef("Cutscenes: %s: its hide list names no object of the game's object list", g_playId.c_str());
        else
            Log::Writef("Cutscenes: %s: %d instance%s of its hide list hidden (%d live)", g_playId.c_str(), hidden,
                        hidden == 1 ? "" : "s", found);
    }
}

// The level's NPCs that are not the cutscene's actors, hidden while it plays (Entry::hideNpcs), every second. The NPCs:
// the NpcComponent list (its head retail 0x2D62CE8, as the game walks it at retail 0x70259F; the next one at +0x1A58,
// retail 0x7025CF; the NpcComponent's Instance at +0x3C8, retail 0x4CA7F2: as enemy_ai.cpp reads them). The actors: the
// playing handler's (+0xA0 count, +0xA8 list of 0xF0, the placement key first; keys -1..-255 through the game's
// callback, the others through GetCineActorInstance, as ActorReport finds them).
constexpr std::uintptr_t kRvaNpcList = 0x02D62CE8ull, kRvaNpcListLoad = 0x0070259Full, kRvaNpcListNextUse = 0x007025CFull,
                         kRvaNpcInstanceUse = 0x004CA7F2ull;
constexpr std::uint8_t kNpcListNextUseSig[] = {0x48, 0x8B, 0x9B, 0x58, 0x1A, 0x00, 0x00};
constexpr std::uint8_t kNpcInstanceUseSig[] = {0x48, 0x8B, 0x96, 0xC8, 0x03, 0x00, 0x00};
constexpr std::size_t kNpcNext = 0x1A58, kNpcInstance = 0x3C8;
constexpr int kMaxActors = 256, kMaxNpcs = 2048;
ULONGLONG g_npcHideAt = 0;
bool g_npcHideLogged = false;

bool NpcListReady() {
    static const bool ready = [] {
        const auto* load = reinterpret_cast<const std::uint8_t*>(GameBuild::Base() + kRvaNpcListLoad);
        const bool list = load[0] == 0x48 && load[1] == 0x8B && load[2] == 0x1D &&
                          kRvaNpcListLoad + 7 + static_cast<std::intptr_t>(*reinterpret_cast<const std::int32_t*>(load + 3)) ==
                              kRvaNpcList;
        const bool ok = list && GameBuild::VerifyBytes(kRvaNpcListNextUse, kNpcListNextUseSig, sizeof(kNpcListNextUseSig)) &&
                        GameBuild::VerifyBytes(kRvaNpcInstanceUse, kNpcInstanceUseSig, sizeof(kNpcInstanceUseSig));
        if (!ok) Log::Write("Cutscenes: the NPC list's code does not match; the level's NPCs are not hidden for cutscenes");
        return ok;
    }();
    return ready;
}

// The playing cutscene's actors that are there (their instances). -1: no handler. SEH only.
int CineActorInstances(void** out, int capacity) noexcept {
    __try {
        auto* handler = *reinterpret_cast<std::uint8_t**>(GameBuild::Base() + kRvaCurrentCineHandler);
        if (!handler) return -1;
        using ActorFn = void*(__fastcall*)(void*, std::uint64_t, int, std::uint64_t*);
        using SpecialFn = void*(__fastcall*)(int, int);
        const std::uintptr_t base = GameBuild::Base();
        const int count = *reinterpret_cast<int*>(handler + 0xA0);
        auto* actors = *reinterpret_cast<std::uint8_t**>(handler + 0xA8);
        auto special = *reinterpret_cast<SpecialFn*>(base + kRvaCineInstanceCallback);
        const int unit = *reinterpret_cast<int*>(handler + 0x2C);
        int found = 0;
        for (int i = 0; i < count && actors && found < capacity; ++i) {
            const std::uint64_t key = *reinterpret_cast<std::uint64_t*>(actors + std::size_t(i) * 0xF0);
            const std::int64_t signedKey = static_cast<std::int64_t>(key);
            void* instance = nullptr;
            if (signedKey < 0 && signedKey > -256) {
                instance = special ? special(static_cast<int>(signedKey), -1) : nullptr;
            } else {
                std::uint64_t placement = 0;
                instance = reinterpret_cast<ActorFn>(base + kRvaGetCineActor)(handler, key, unit, &placement);
            }
            if (instance) out[found++] = instance;
        }
        return found;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
}

// The NPCs' instances (the list's order). SEH only.
int NpcInstances(void** out, int capacity) noexcept {
    __try {
        auto* npc = *reinterpret_cast<std::uint8_t**>(GameBuild::Base() + kRvaNpcList);
        int found = 0;
        for (int n = 0; npc && n < kMaxNpcs && found < capacity; ++n) {
            if (void* instance = *reinterpret_cast<void**>(npc + kNpcInstance)) out[found++] = instance;
            npc = *reinterpret_cast<std::uint8_t**>(npc + kNpcNext);
        }
        return found;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
}

void HideOtherNpcs(ULONGLONG now) {
    if (!g_viewerMode || !g_playHideNpcs || now < g_npcHideAt || !NpcListReady()) return;
    g_npcHideAt = now + 1000;
    static void* actors[kMaxActors];
    static void* npcs[kMaxNpcs];
    const int actorCount = CineActorInstances(actors, kMaxActors);
    if (actorCount < 0) return;
    const int npcCount = NpcInstances(npcs, kMaxNpcs);
    if (npcCount < 0) return;
    int hidden = 0, already = 0, failed = 0;
    for (int i = 0; i < npcCount; ++i) {
        if (std::find(actors, actors + actorCount, npcs[i]) != actors + actorCount) continue;
        const int result = Features::HideInstance(npcs[i]);
        hidden += result > 0;
        already += result == 0;
        failed += result < 0;
    }
    if (!g_npcHideLogged && (hidden > 0 || now - g_phaseSince > 3000)) {
        g_npcHideLogged = true;
        Log::Writef("Cutscenes: %s: the level's NPCs that are not its actors hidden: %d (%d were already, %d could not be; "
                    "%d NPCs, %d actors there)", g_playId.c_str(), hidden, already, failed, npcCount, actorCount);
    }
}

// ---- The level's music before the viewer's cutscene -------------------------------------------------------------------
// 0.9.365 held the music bus at 0 for To the Tower and Locating the Source, and the cutscenes' own music, which goes
// through that bus too, was silent with it (0.9.367). Now the music the level plays at the checkpoint is ended before
// the cutscene's node fires (or before the viewer starts it directly), and what the cutscene starts after that plays.
// The sounds playing are the game's SoundOwners: SoundPlex::sm_Collection (retail 0x2400D90, TR2 0x246F160) points at
// a circular list's head node (next +0, the owner +0x10), as SOUND_StopAllSound (retail 0x3B8B10, TR2 0x3DCB30) walks
// it to end each owner (its end type, retail +0x3F4, TR2 +0x408: 5 with a fade when given one, 6 at once; 0xFF from
// SoundOwner's ctor, retail 0x395970: none yet). An owner's sound group is at +0x94: SoundPlexAssignment's ctor (the
// code at retail 0x395ECA, TR2 0x3B9163) sets it from the plex's assignment and attaches the owner to that group
// (gSoundGroups, retail 0xFF7380: +0 the groups, 0x1C0 each, parent index +0x1B0, name +0x148; +8 their count). The
// music group is globalSoundInfo (retail 0x233C488) +0x68 [0], as SOUND_InitFromGlobalSoundInfo (retail 0x3B7BC0)
// makes the music volume's controller for it. Owners in that group or under it are given 5 (ended with a fade).
// "keepLevelMusic" leaves them.
constexpr std::uintptr_t kRvaGlobalSoundInfo = 0x0233C488ull, kRvaSoundGroups = 0x00FF7380ull,
                         kRvaSoundOwners = 0x02400D90ull, kRvaStopAllSoundLoad = 0x003B8B29ull,
                         kRvaStopAllSoundEnd = 0x003B8B4Dull, kRvaAssignmentGroup = 0x00395ECAull,
                         kRvaAssignmentGroupsLoad = 0x00395EDFull, kRvaSoundInitInfoLoad = 0x003B7BD3ull,
                         kRvaSoundInitGroupsLea = 0x003B7BDAull;
constexpr std::uint8_t kStopAllSoundEndSig[] = {0xC6, 0x81, 0xF4, 0x03, 0x00, 0x00, 0x05};
constexpr std::uint8_t kAssignmentGroupSig[] = {0x49, 0x8B, 0x47, 0x30, 0x8B, 0x48, 0x28,
                                               0x89, 0x8D, 0x94, 0x00, 0x00, 0x00};
constexpr std::size_t kSoundGroupSize = 0x1C0, kSoundGroupParent = 0x1B0;
constexpr std::size_t kNodeOwner = 0x10, kOwnerGroup = 0x94, kOwnerPlexId = 0x34C, kOwnerEnd = 0x3F4;
constexpr std::uint8_t kOwnerEndNone = 0xFF, kOwnerEndFade = 5;
constexpr int kMaxSoundOwners = 4096;
int g_musicCodeState = 0;   // 0 not checked, 1 as expected, -1 not (logged)
int g_musicLogs = 0;         // (the music group's sounds logged 5 s into the cutscene, once)
ULONGLONG g_musicLogAt = 0;

// "mov reg, [rip + x]" (48 8B, or 48 8D for lea) at rva whose x is target.
bool RipOperand(std::uintptr_t rva, std::uint8_t opcode, std::uintptr_t target) {
    const auto* code = reinterpret_cast<const std::uint8_t*>(GameBuild::Base() + rva);
    return code[0] == 0x48 && code[1] == opcode &&
           rva + 7 + static_cast<std::intptr_t>(*reinterpret_cast<const std::int32_t*>(code + 3)) == target;
}

bool MusicCodeMatches() {
    if (g_musicCodeState) return g_musicCodeState > 0;
    const bool ok = RipOperand(kRvaStopAllSoundLoad, 0x8B, kRvaSoundOwners) &&
                    RipOperand(kRvaAssignmentGroupsLoad, 0x8B, kRvaSoundGroups) &&
                    RipOperand(kRvaSoundInitInfoLoad, 0x8B, kRvaGlobalSoundInfo) &&
                    RipOperand(kRvaSoundInitGroupsLea, 0x8D, kRvaSoundGroups) &&
                    GameBuild::VerifyBytes(kRvaStopAllSoundEnd, kStopAllSoundEndSig, sizeof(kStopAllSoundEndSig)) &&
                    GameBuild::VerifyBytes(kRvaAssignmentGroup, kAssignmentGroupSig, sizeof(kAssignmentGroupSig));
    g_musicCodeState = ok ? 1 : -1;
    if (!ok)
        Log::Write("Cutscenes: the game's sound code is not where expected; the level's music is not ended for cutscenes");
    return ok;
}

// The group, or one of its parents, is the music group. (Under the caller's SEH.)
bool InMusicGroup(const std::uint8_t* groups, unsigned count, unsigned group, unsigned music) {
    for (int steps = 0; group < count && steps < 32; ++steps) {
        if (group == music) return true;
        const auto* entry = groups + std::size_t(group) * kSoundGroupSize;
        const unsigned parent = *reinterpret_cast<const unsigned*>(entry + kSoundGroupParent);
        if (parent == group || group == 0) return false;
        group = parent;
    }
    return false;
}

struct MusicSounds {
    int owners = 0, music = 0, ended = 0;   // the sounds playing, those in the music group, those ended now
    unsigned ids[8]{};                      // the DTPs of the music's plexes (for the log)
    int idCount = 0;
};
// The sounds in the music group (ended with a fade when end). SEH only.
bool MusicGroupSounds(MusicSounds& out, bool end) noexcept {
    __try {
        const std::uintptr_t base = GameBuild::Base();
        const auto* info = *reinterpret_cast<const std::uint8_t* const*>(base + kRvaGlobalSoundInfo);
        const auto* groups = *reinterpret_cast<const std::uint8_t* const*>(base + kRvaSoundGroups);
        const unsigned count = *reinterpret_cast<const unsigned*>(base + kRvaSoundGroups + 8);
        const unsigned* named = info ? *reinterpret_cast<const unsigned* const*>(info + 0x68) : nullptr;
        auto* head = *reinterpret_cast<std::uint8_t**>(base + kRvaSoundOwners);
        if (!groups || !named || !count || !head) return false;
        const unsigned music = named[0];
        if (music == 0 || music >= count) return false;   // (the master group: every sound is under it)
        int steps = 0;
        for (auto* node = *reinterpret_cast<std::uint8_t**>(head); node && node != head && steps < kMaxSoundOwners;
             node = *reinterpret_cast<std::uint8_t**>(node), ++steps) {
            auto* owner = *reinterpret_cast<std::uint8_t**>(node + kNodeOwner);
            if (!owner) continue;
            ++out.owners;
            if (!InMusicGroup(groups, count, *reinterpret_cast<const unsigned*>(owner + kOwnerGroup), music)) continue;
            ++out.music;
            if (out.idCount < 8) out.ids[out.idCount++] = *reinterpret_cast<const unsigned*>(owner + kOwnerPlexId);
            if (end && owner[kOwnerEnd] == kOwnerEndNone) {
                owner[kOwnerEnd] = kOwnerEndFade;
                ++out.ended;
            }
        }
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

std::string MusicIds(const MusicSounds& sounds) {
    std::string text;   // (the owners' +0x34C: the DTP their plex is in, the level's script for its music)
    for (int i = 0; i < sounds.idCount; ++i) text += (i ? ", " : "") + std::to_string(sounds.ids[i]);
    if (sounds.music > sounds.idCount) text += ", ...";
    return text.empty() ? "none" : text;
}

// Game thread: before the viewer's cutscene starts (its node fires, or it is started directly).
void EndLevelMusic(const char* when) {
    g_musicLogs = 0;
    g_musicLogAt = GetTickCount64() + 5000;
    if (!MusicCodeMatches()) return;
    MusicSounds sounds;
    if (!MusicGroupSounds(sounds, !g_playKeepMusic)) {
        Log::Writef("Cutscenes: %s: the sounds playing could not be read; the level's music is left", g_playId.c_str());
        return;
    }
    if (g_playKeepMusic)
        Log::Writef("Cutscenes: %s: the level's music kept (keepLevelMusic): %d of the %d sounds playing %s are in the "
                    "music group (from DTP %s)", g_playId.c_str(), sounds.music, sounds.owners, when,
                    MusicIds(sounds).c_str());
    else
        Log::Writef("Cutscenes: %s: the level's music ended %s: %d sounds of the music group faded out (%d there, "
                    "from DTP %s; %d sounds playing)", g_playId.c_str(), when, sounds.ended, sounds.music,
                    MusicIds(sounds).c_str(), sounds.owners);
}

// The music group's sounds 5 s into the cutscene (its own music, or the level's started again): logged once.
void LogCutsceneMusic(ULONGLONG now) {
    if (g_musicLogs || !g_musicLogAt || now < g_musicLogAt || g_musicCodeState <= 0) return;
    g_musicLogs = 1;
    MusicSounds sounds;
    if (MusicGroupSounds(sounds, false))
        Log::Writef("Cutscenes: %s: the music group's sounds 5 s in: %d (from DTP %s; %d sounds playing)",
                    g_playId.c_str(), sounds.music, MusicIds(sounds).c_str(), sounds.owners);
}

// What stands around Lara when the cutscene starts and 5 s later (for hide lists): logged for viewer plays.
int g_nearLogs = 0;
ULONGLONG g_nearLogAt = 0;
void LogInstancesNearLara(ULONGLONG now) {
    if (!g_viewerMode || g_nearLogs >= 2 || now < g_nearLogAt) return;
    g_nearLogAt = now + 5000;
    ++g_nearLogs;
    Log::Writef("Cutscenes: %s: instances near Lara (%s): %s", g_playId.c_str(), g_nearLogs == 1 ? "its start" : "5 s in",
                Features::DescribeInstancesNearPlayer(15.0f, 60).c_str());
}

// What moves (In Through the Out Door, 0.9.391 to 0.9.395: rocks falling, loud, at the viewer's scene; not in the
// story's. 0.9.395's log: only Lara and what she carries moved, the rocks were the tremors' particles): the live
// instances of two sweeps of the pool (Features::CopyInstanceSpots, slot order) compared by full id.
struct Mover {
    std::uint64_t id;
    std::uint32_t zone;
    float moved, dz, fromLara;   // (engine units, cm; fromLara -1: not known)
};
// The instances of `after` that moved more than minMove since `before`, most first.
std::vector<Mover> Movers(const std::vector<Features::InstanceSpot>& before, const std::vector<Features::InstanceSpot>& after,
                          float minMove) {
    std::vector<Mover> out;
    float lara[3] = {};
    const bool haveLara = Features::PlayerPosition(lara);
    std::size_t i = 0;
    for (const Features::InstanceSpot& b : after) {
        const std::uint32_t slot = std::uint32_t(b.id);
        while (i < before.size() && std::uint32_t(before[i].id) < slot) ++i;
        if (i >= before.size() || before[i].id != b.id) continue;   // (born between the two)
        const Features::InstanceSpot& a = before[i];
        const float dx = b.at[0] - a.at[0], dy = b.at[1] - a.at[1], dz = b.at[2] - a.at[2];
        const float moved = std::sqrt(dx * dx + dy * dy + dz * dz);
        if (!(moved > minMove)) continue;
        float from = -1.0f;
        if (haveLara) {
            const float lx = b.at[0] - lara[0], ly = b.at[1] - lara[1], lz = b.at[2] - lara[2];
            from = std::sqrt(lx * lx + ly * ly + lz * lz);
        }
        out.push_back({b.id, b.zone, moved, dz, from});
    }
    std::sort(out.begin(), out.end(), [](const Mover& x, const Mover& y) { return x.moved > y.moved; });
    return out;
}
std::string MoversText(const std::vector<Mover>& movers, int max) {
    std::string text;
    char item[192];
    int shown = 0;
    for (const Mover& m : movers) {
        if (shown++ >= max) break;
        snprintf(item, sizeof(item), "%s%s (slot %u, moved %.0f cm, up %.0f cm, %.1f m from Lara)", text.empty() ? "" : "; ",
                 Features::ObjectNameOfZone(m.zone).c_str(), std::uint32_t(m.id), m.moved, m.dz, m.fromLara / 100.0f);
        text += item;
    }
    if (int(movers.size()) > max) text += "; " + std::to_string(movers.size() - max) + " more";
    return text.empty() ? "nothing" : text;
}

// Logged for viewer plays: what moves over 0.5 s at its start and 5 s in.
int g_moveStep = 0;
ULONGLONG g_moveAt = 0;
std::vector<Features::InstanceSpot> g_moveBefore;
void LogMovers(ULONGLONG now) {
    if (!g_viewerMode || g_moveStep >= 4) return;
    if (g_moveStep == 0 || g_moveStep == 2) {
        if (g_moveStep == 2 && now < g_moveAt + 5000) return;
        Features::CopyInstanceSpots(g_moveBefore);
        g_moveAt = now;
        ++g_moveStep;
        return;
    }
    if (now - g_moveAt < 500) return;
    std::vector<Features::InstanceSpot> after;
    Features::CopyInstanceSpots(after);
    const std::vector<Mover> movers = Movers(g_moveBefore, after, 5.0f);
    Log::Writef("Cutscenes: %s: what moved %s (over %.1f s, %d of %d instances): %s", g_playId.c_str(),
                g_moveStep == 1 ? "at its start" : "5 s in", (now - g_moveAt) / 1000.0, int(movers.size()),
                int(after.size()), MoversText(movers, 10).c_str());
    ++g_moveStep;
}

// How the cutscene came (see Entry::trigger): remembered in cutscenes.json.
void LearnTrigger(const std::string& id, const char* trigger) {
    std::lock_guard lock(g_catalogMutex);
    for (Entry& e : g_catalog)
        if (e.id == id && !e.startUnit.empty() && e.trigger != trigger) {
            e.trigger = trigger;
            WriteCatalog(g_catalog);
            Log::Writef("Cutscenes: %s noted as started by %s", id.c_str(),
                        std::strcmp(trigger, "node") == 0 ? "its level node" : "the level itself");
            break;
        }
}

// Ours is over and the story plays the next of its chain (Entry::then) right after it: that one becomes the one watched
// for. When it plays already, the play goes on (LevelPlaying); otherwise Watching waits a little for the level to start
// it, then fires its node as for ours. The viewer keeps the picture through a short gap (CurrentStage) and shows no
// fade-out at the end of one that has another after it (g_secondsLeft -1).
bool ChainPending() { return g_chainStep < g_chain.size(); }
void AdvanceChain(ULONGLONG now, bool alreadyPlaying) {
    const std::string next = g_chain[g_chainStep++];
    Log::Writef("Cutscenes: %s over; the story goes on with %s%s", g_playId.c_str(), next.c_str(),
                alreadyPlaying ? " (it plays)" : " (waited for)");
    g_playId = next;
    g_playVia.clear();
    g_fireId.clear();
    g_viaUsed = false;
    g_skipName.clear();
    g_skipTries = 0;
    g_skipAt = 0;
    g_nodeFires = 0;
    g_storyEventAt = 0;
    g_storyEventWaitLogged = false;
    g_storyRounds = 0;
    g_storyRoundAt = 0;
    g_nodeMissingLogged = g_nodeBusyLogged = false;
    g_nodeReports = g_actorReports = 0;
    g_stallFrame = -1.0f;
    g_stallLogged = g_lengthLogged = false;
    g_birthWaitSince = 0;
    g_birthLogged = false;
    g_hideAt = 0;
    g_hideLogged = g_removeLogged = false;
    g_nearLogs = 0;
    g_moveStep = 0;
    g_musicLogAt = 0;
    g_nearLogAt = 0;
    g_layerUnits.clear();
    g_storyNodes.clear();
    g_turnedOn.clear();   // (the one before's layers are the story's now: not turned on again if it turns them off)
    g_turnedOff.clear();
    g_activeLayersLogged = false;
    g_layersOn = 0;
    g_layersAt = 0;
    g_noNodeSince = 0;
    g_directStart = g_directMissLogged = false;
    g_secondsLeft = -1.0f;
    g_cineUnit.clear();
    g_playLayers.clear();
    g_playLayersOff.clear();
    g_playLayersOffAtStart.clear();
    g_layersOffAtStartDone = false;
    g_playUnits.clear();
    g_unitsQueued.clear();
    g_unitsQueuedAt = 0;
    g_unitsTimeoutLogged = false;
    {
        std::lock_guard lock(g_catalogMutex);
        for (const Entry& e : g_catalog)
            if (e.id == next) {
                g_cineUnit = e.unit;
                g_playLayers = e.layers;
                g_playLayersOff = e.layersOff;
                g_playLayersOffAtStart = e.layersOffAtStart;
                g_playUnits = e.units;
                g_playStoryEvent.clear();   // (the next of a chain comes after the one before it)
                g_playHideNpcs = g_playHideNpcs || e.hideNpcs;

            }
    }
    g_phaseSince = g_quietSince = g_lastFrameLog = now;
    g_chainGapSince = alreadyPlaying ? 0 : now;
}
// The seconds left of the one playing, for the viewer's fade-out: none while another of its chain comes after it.
void SetSecondsLeft(float seconds) { g_secondsLeft = ChainPending() ? -1.0f : seconds; }

// The cutscene's first frame: how long after the load's end, and what the time went to (once per viewer play).
void LogBlack(ULONGLONG now) {
    if (g_waitTick) g_waitMs[std::size_t(g_waitReason)] += now - g_waitTick;
    g_waitTick = 0;
    if (g_blackLogged || !g_viewerMode || g_chainStep) return;
    g_blackLogged = true;
    if (!g_loadEndAt) {
        Log::Writef("Cutscenes: %s: its first frame came before the game's load ended (%.1f s after the level was asked for)",
                    g_playId.c_str(), (now - g_loadAskedAt) / 1000.0);
        return;
    }
    std::string parts;
    for (std::size_t i = 0; i < std::size_t(Wait::Count); ++i) {
        if (g_waitMs[i] < 50) continue;
        char part[96];
        snprintf(part, sizeof(part), "%s%s %.1f s", parts.empty() ? "" : ", ", kWaitNames[i], g_waitMs[i] / 1000.0);
        parts += part;
    }
    Log::Writef("Cutscenes: %s: its first frame %.1f s after the game's load ended (%.1f s after the level was asked for); "
                "after the load: %s", g_playId.c_str(), (now - g_loadEndAt) / 1000.0, (now - g_loadAskedAt) / 1000.0,
                parts.empty() ? "nothing" : parts.c_str());
}

void TickGameThread() {
    if (!g_gameReady) return;
    WriteChapterSelectList();
    Request request;
    {
        std::lock_guard lock(g_requestMutex);
        if (g_request.pending) {
            request = g_request;
            g_request = Request{};
        }
    }
    const ULONGLONG now = GetTickCount64();
    if (request.pending) {
        if (request.stop) {
            if (g_phase == Phase::Playing) StopCine();   // (only ours)
            g_phase = Phase::Idle;
            SetStatus("stopped");
        } else if (g_phase == Phase::Idle) {
            g_playId = request.id;
            g_playUnit = request.unit;
            g_playVia = request.via;
            g_fireId.clear();
            g_viaUsed = false;
            g_skipName.clear();
            g_skipTries = 0;
            g_skipAt = 0;
            g_reloadLogged = false;
            g_cineUnit.clear();
            g_playLayers.clear();
            g_playLayersOff.clear();
            g_playLayersOffAtStart.clear();
            g_layersOffAtStartDone = false;
            g_playUnits.clear();
            g_unitsQueued.clear();
            g_unitsQueuedAt = 0;
            g_unitsTimeoutLogged = false;
            g_turnedOn.clear();
            g_turnedOff.clear();
            g_chain.clear();
            g_chainStep = 0;
            g_chainGapSince = 0;
            g_nodeKnown = false;
            g_playHideNpcs = false;
            g_playKeepMusic = false;
            g_npcHideAt = 0;
            g_npcHideLogged = false;
            {
                std::lock_guard lock(g_catalogMutex);
                std::string viaTrigger;
                for (const Entry& e : g_catalog) {
                    if (e.id == request.id) {
                        g_cineUnit = e.unit;
                        g_playLayers = e.layers;
                        g_playLayersOff = e.layersOff;
                        g_playLayersOffAtStart = e.layersOffAtStart;
                        g_playUnits = e.units;
                        g_playStoryEvent = e.storyEvent;
                        g_chain = e.then;
                        g_nodeKnown = e.trigger == "node";
                        g_playHideNpcs = e.hideNpcs;
                        g_playKeepMusic = e.keepLevelMusic;
                    }
                    if (!request.via.empty() && e.id == request.via) viaTrigger = e.trigger;
                }
                if (!request.via.empty()) g_nodeKnown = viaTrigger == "node";
            }
            g_layerUnits.clear();
            g_storyNodes.clear();
            g_activeLayersLogged = false;
            g_layersOn = 0;
            g_layersAt = 0;
            g_noNodeSince = 0;
            g_directStart = g_directMissLogged = false;
            g_viaChapter = request.chapterUnit != 0 || request.storyPlays;
            g_lastSeenCine.clear();
            g_nodeFires = 0;
            g_storyEventAt = 0;
            g_storyEventWaitLogged = false;
            g_storyRounds = 0;
            g_storyRoundAt = 0;
            g_watchSince = 0;
            std::fill(std::begin(g_waitMs), std::end(g_waitMs), 0ull);
            g_waitReason = Wait::None;
            g_waitTick = g_loadEndAt = 0;
            g_blackLogged = false;
            g_nodeMissingLogged = g_nodeBusyLogged = false;
            g_nodeReports = g_actorReports = 0;
            g_stallFrame = -1.0f;
            g_stallLogged = g_lengthLogged = false;
            g_birthWaitSince = 0;
            g_birthLogged = false;
            g_hideAt = 0;
            g_hideLogged = g_removeLogged = false;
            g_nearLogs = 0;
            g_moveStep = 0;
            g_musicLogAt = 0;
            g_nearLogAt = 0;
            const int unitId = LoadedUnitId(request.unit.c_str());
            if (request.chapterUnit) {
                if (Features::RequestUnitLoad(request.chapterUnit, request.unit.c_str(), request.runnable.c_str())) {
                    g_phase = Phase::LoadingUnit;
                    g_phaseSince = g_loadAskedAt = now;
                    SetStatus("loading %s through Chapter Select (runnable %s) for %s", request.unit.c_str(),
                              request.runnable.empty() ? "<none>" : request.runnable.c_str(), request.id.c_str());
                } else {
                    SetStatus("the Chapter Select load of %s was refused now", request.unit.c_str());
                }
            } else if (unitId) {
                Play(request.id, unitId);
            } else if (request.load) {
                if (Features::RequestUnitLoadByName(request.unit.c_str())) {
                    g_phase = Phase::LoadingUnit;
                    g_phaseSince = g_loadAskedAt = now;
                    SetStatus("loading %s for %s", request.unit.c_str(), request.id.c_str());
                } else {
                    SetStatus("%s could not be loaded now", request.unit.c_str());
                }
            } else {
                SetStatus("%s's unit %s is not loaded", request.id.c_str(), request.unit.c_str());
            }
        }
    }

    if (g_abortRequested.exchange(false)) {
        if (g_phase == Phase::Playing) StopCine();
        const bool inGame = g_phase != Phase::Idle || LoadedUnitId(g_playUnit.c_str());
        g_phase = Phase::Idle;
        g_secondsLeft = -1.0f;
        if (inGame && ReturnToMainMenu()) SetStatus("%s left; back to the main menu", g_playId.c_str());
    }

    // a cutscene the game plays on its own (not the viewer's, not the test panel's): the scene's state logged at its start.
    // The one playing while the viewer (or the test panel) is busy is theirs: the viewer's, still playing for a moment
    // after it left for the main menu, was taken for the story's in 0.9.378's log.
    static std::string storyCine;
    if (g_gameReady) {
        char name[128];
        float frame = 0;
        const bool playing = CurrentCine(name, sizeof(name), &frame);
        if (g_phase != Phase::Idle) {
            storyCine = playing ? name : "";
        } else if (!playing) {
            storyCine.clear();
        } else if (name[0] && storyCine != name) {
            storyCine = name;
            Log::Writef("Cutscenes: the game plays %s (the story's, outside the viewer; frame %.0f)", name, frame);
            LogSceneState(name, "at its start", true);
        }
    }

    if (g_waitTick) {   // (the time since the last tick, to what the viewer waited for then)
        g_waitMs[std::size_t(g_waitReason)] += now - g_waitTick;
        g_waitTick = now;
    }
    g_waitReason = Wait::None;
    switch (g_phase) {
        case Phase::Idle:
            break;
        case Phase::LoadingUnit:
            if (!Features::UnitLoadInProgress() && LoadedUnitId(g_playUnit.c_str())) {
                g_phase = Phase::Settling;
                g_phaseSince = now;
            } else if (now - g_phaseSince > 120000) {
                g_phase = Phase::Idle;
                SetStatus("%s did not load in 2 minutes", g_playUnit.c_str());
            }
            break;
        case Phase::Settling: {
            // a cutscene already playing while the game still counts as loading (the unit starts it at once)
            char name[128];
            float frame = 0;
            if (CurrentCine(name, sizeof(name), &frame)) {
                if (g_lastSeenCine != name) {
                    g_lastSeenCine = name;
                    Log::Writef("Cutscenes: the game plays %s (frame %.0f) before its loading ended", name[0] ? name : "(no name)",
                                frame);
                }
                if (name[0] && SameCine(name, g_playId)) {
                    g_phase = Phase::LevelPlaying;
                    g_phaseSince = g_lastFrameLog = now;
                    TurnOffLayersAtStart();
                    SetStatus("%s: the game started it", g_playId.c_str());
                    if (g_viaChapter) LearnTrigger(g_playId, "load");
                    LogBlack(now);
                    break;
                }
            }
            // the game's loading over (GameStateLoading left) for 0.5 s (2 s up to 0.9.363: the viewer's black stood
            // that long after the loading screen)
            if (Features::LoadingProfilerSessionActive()) {
                g_phaseSince = now;
                if (now - g_lastFrameLog >= 10000) {
                    g_lastFrameLog = now;
                    Log::Writef("Cutscenes: %s: the game is still loading (loading state %d, %.0f s)", g_playUnit.c_str(),
                                Features::LoadingStateNow(), (now - g_loadAskedAt) / 1000.0);
                }
                if (now - g_loadAskedAt > 180000) {
                    g_phase = Phase::Idle;
                    SetStatus("%s: the game's loading has not ended in 3 minutes", g_playUnit.c_str());
                }
                break;
            }
            if (!g_loadEndAt) g_loadEndAt = g_waitTick = now;
            g_waitReason = Wait::Settling;
            if (now - g_phaseSince < 500) break;
            g_phase = Phase::Watching;
            g_phaseSince = g_quietSince = g_watchSince = now;
            SetStatus("%s loaded; watching whether it plays a cutscene itself", g_playUnit.c_str());
            break;
        }
        case Phase::Watching: {
            HideForCutscene(now);
            char name[128];
            float frame = 0;
            if (CurrentCine(name, sizeof(name), &frame)) {
                if (g_lastSeenCine != name) {
                    g_lastSeenCine = name;
                    Log::Writef("Cutscenes: the game plays %s (frame %.0f)", name[0] ? name : "(no name)", frame);
                }
                if (SameCine(name, g_playId) || !name[0]) {
                    g_phase = Phase::LevelPlaying;
                    g_phaseSince = g_lastFrameLog = now;
                    g_chainGapSince = 0;
                    TurnOffLayersAtStart();   // (what the story switches off between its node and its first frame)
                    SetStatus("%s: the game started it%s", g_playId.c_str(),
                              g_chainStep ? " (after the one before it)"
                                          : (g_viaUsed ? " (after its way there)" : (g_nodeFires ? " (its node fired)" : "")));
                    if (g_viaChapter && name[0] && !g_viaUsed && !g_chainStep)
                        LearnTrigger(g_playId, g_nodeFires ? "node" : "load");
                    LogBlack(now);
                } else if (g_viewerMode) {
                    g_waitReason = Wait::OtherCine;
                    SkipOtherCine(name, frame, now);
                }
                g_quietSince = now;
                break;   // (another one first: watched until ours comes)
            }
            // the game loads again: the story goes on in another unit (Execution's, after The Reckoning)
            if (Features::LoadingProfilerSessionActive()) {
                if (!g_reloadLogged) {
                    g_reloadLogged = true;
                    Log::Writef("Cutscenes: the game loads again while %s is awaited (the story goes on)", g_playId.c_str());
                }
                g_waitReason = Wait::Loading;
                g_phaseSince = g_quietSince = now;
                break;
            }
            if (g_viaChapter) {
                // a checkpoint before the cutscene: the story waits for Lara somewhere. After 3 s without a cutscene
                // (8 s when the level already played one: its story may go on to ours by itself, as The Enemy after
                // igc_bear_valley_go_to_sleep; or 25 s after the load whatever plays: some levels keep one running)
                // the level's own node for ours is fired (again every 5 s, 3 times). When ours is known to come by its
                // node (Entry::trigger "node"), 0.5 s (1.5 s after another one): the level does not start it itself, and
                // the viewer's black after the loading screen stays short. The next of a chain: 1.5 s (the story may
                // start it itself).
                ULONGLONG quietNeeded = g_lastSeenCine.empty() ? 3000 : 8000;
                if (g_nodeKnown) quietNeeded = g_lastSeenCine.empty() ? 500 : 1500;
                if (g_chainStep) quietNeeded = 1500;
                const bool quiet = now - g_quietSince >= quietNeeded || now - g_phaseSince >= 25000;
                if (g_nodeHold.load()) g_phaseSince = std::max(g_phaseSince, now - quietNeeded);   // (the viewer swaps the outfit first)
                g_waitReason = g_nodeHold.load() ? Wait::Outfit
                                                 : (g_storyEventAt ? Wait::StoryWay : (g_nodeFires ? Wait::Stream : Wait::Quiet));
                // the cutscene's own stream layer (its props and actors) turned on in the units that may play it, once
                // the viewer's outfit swap is over (the swap's respawn, at the same moment, left Trust Issues' layer off
                // and Meet the Remnant crashed as its node fired, 0.9.362)
                if (!g_nodeHold.load()) {
                    LookForCineLayers(LoadedUnitId(g_playUnit.c_str()), now);
                    if (!g_cineUnit.empty()) LookForCineLayers(LoadedUnitId(g_cineUnit.c_str()), now);
                }
                if (!g_nodeHold.load() && quiet && g_nodeFires < 3 && now - g_phaseSince >= quietNeeded + 5000u * g_nodeFires &&
                    (!g_storyEventAt || now - g_storyEventAt >= 15000)) {
                    NodeFind find = FindCineNode(g_playId);
                    g_fireId = g_playId;
                    if (find.node) LookForCineLayers(find.unit, now);
                    if (!find.node && !g_playVia.empty()) {   // (ours plays in a unit the story loads after the via)
                        const NodeFind via = FindCineNode(g_playVia);
                        if (via.node) {
                            find = via;
                            g_fireId = g_playVia;
                            if (!g_viaUsed) Log::Writef("Cutscenes: %s: no node of it here; its way there, %s, is started",
                                                        g_playId.c_str(), g_playVia.c_str());
                            g_viaUsed = true;
                        }
                    }
                    if (find.node) {
                        g_noNodeSince = 0;
                        LookForStoryLayers(find.node, g_fireId, now);
                    } else if (!g_noNodeSince) {
                        g_noNodeSince = now;
                    }
                    if (!find.node) g_waitReason = Wait::NoNode;
                    // (first the level's placements born, 6 s at most: what the cutscene binds at its start is there;
                    // 20 s once layers of ours were turned on, at least 1 s after: their loads are queued first)
                    const bool birthed = g_agReady ? AllPlacementsBirthed() : true;
                    if (!birthed && g_birthWaitSince == 0) g_birthWaitSince = now;
                    const bool layersSettling = g_layersOn && now - g_layersAt < 1000;
                    const ULONGLONG birthLimit = g_layersOn ? 20000 : 6000;
                    const bool unitsReady = PlayUnitsReady(now);
                    if (!unitsReady) g_birthWaitSince = 0;   // (the placements counted once its units are in)
                    bool hold = find.node && (!unitsReady || layersSettling || (!birthed && now - g_birthWaitSince < birthLimit));
                    // a story event: the level's delays in the graphs it reaches run out first, then 0.5 s for what
                    // they started (again while more are running, 3 rounds at most)
                    bool storyWait = false;
                    if (find.node && !hold && g_nodeFires == 0 && !g_playStoryEvent.empty() && !g_storyEventAt &&
                        g_fireId == g_playId) {
                        if (g_watchSince && now - g_watchSince < kStoryEventAfterLoad) {
                            storyWait = true;
                            if (!g_storyEventWaitLogged) {
                                g_storyEventWaitLogged = true;
                                Log::Writef("Cutscenes: %s: its story event waits until %.1f s after the load (%.1f s now)",
                                            g_playId.c_str(), kStoryEventAfterLoad / 1000.0, (now - g_watchSince) / 1000.0);
                            }
                        } else if (g_storyRoundAt && now - g_storyRoundAt < kStoryRoundMs) {
                            storyWait = true;
                        } else if (g_storyRounds < kStoryRounds) {
                            const int ran = RunOutStoryDelaysFor(find.node, g_storyRounds);
                            ++g_storyRounds;
                            if (ran > 0) {
                                g_storyRoundAt = now;
                                storyWait = true;
                            } else {
                                g_storyRounds = kStoryRounds;
                            }
                        }
                        if (storyWait) hold = true;
                    }
                    const bool layersOff = find.node && !hold && g_nodeFires == 0 && !LayersStillOn(now);
                    if (layersOff) hold = true;
                    if (hold)
                        g_waitReason = !unitsReady ? Wait::Units
                                       : layersSettling || layersOff ? Wait::Layers
                                       : storyWait ? Wait::StoryEventWait
                                                   : Wait::Placements;
                    if (hold) {
                        g_phaseSince = std::max(g_phaseSince, now - quietNeeded - 5000u * g_nodeFires);
                    } else if (find.node) {
                        if (g_birthWaitSince && !g_birthLogged) {
                            g_birthLogged = true;
                            Log::Writef("Cutscenes: %s: %s after %.1f s", g_playId.c_str(),
                                        birthed ? "the level's placements born" : "placements still being born",
                                        (now - g_birthWaitSince) / 1000.0);
                        }
                        if (g_nodeFires == 0 && !g_chainStep) EndLevelMusic("before its node fires");
                        // the camera's shakes stopped, its own shake (its camera settings') forgotten: a camera
                        // shake plays over a cutscene's camera too (CameraBoneCameraController::Process applies the
                        // camera's CameraShake). Whispers in the Dark swayed in 0.9.374; the story changes the camera
                        // on the way (bv_bear_valley's campsite graph: message ResetCampsiteStumbleCam -> 3 s ->
                        // Camera_ChangeOrResetStack; a camera change ends the old camera's own shake, retail
                        // 0x158F51 to 0x158FC2), which the viewer's start skips.
                        if (g_nodeFires == 0 && g_viewerMode) {
                            std::uint32_t own = 0;
                            const int stopped = Features::CameraShakesPlaying(true, &own);
                            if (stopped != 0 || own)
                                Log::Writef("Cutscenes: %s: the camera's shakes stopped before its node fires: %d playing, "
                                            "its camera's own shake %u", g_playId.c_str(), stopped, own);
                        }
                        // the story's event first, when it has one (Retreat: the mini-boss fight's end): the scene
                        // then comes the story's way; its node is fired directly only if it has not come in 15 s
                        if (g_nodeFires == 0 && !g_playStoryEvent.empty() && !g_storyEventAt && g_fireId == g_playId &&
                            SendStoryEventFor(find.node)) {
                            g_storyEventAt = now;
                            ++g_nodeFires;
                            g_nodeReportAt = now + 1000;
                            break;
                        }
                        if (g_storyEventAt)
                            Log::Writef("Cutscenes: %s: not come %.0f s after the story's event; its node is fired directly",
                                        g_playId.c_str(), (now - g_storyEventAt) / 1000.0);
                        char graphText[1536];
                        int fired = g_nodeFires == 0 ? FireCinePlayThroughGraph(find.node, graphText, sizeof(graphText)) : 3;
                        if (g_nodeFires == 0 && graphText[0]) Log::Writef("Cutscenes: %s: its graph: %s", g_fireId.c_str(), graphText);
                        if (fired == 0) {
                            Log::Writef("Cutscenes: %s: fired through the output that drives it", g_fireId.c_str());
                        } else if (fired != 1) {
                            fired = FireCineNodePlay(find.node);
                        }
                        if (fired != 1) {
                            ++g_nodeFires;
                            g_nodeReportAt = now + 1000;
                        }
                        if (fired != 1 || !g_nodeBusyLogged)
                            Log::Writef("Cutscenes: %s: the level's cutscene node %p (unit %d, index %d, state %d; %d nodes) %s",
                                        g_fireId.c_str(), find.node, find.unit, find.index, find.state, find.total,
                                        fired == 0 ? "fired" : (fired == 1 ? "busy (it plays or waits already)" : "faulted"));
                        if (fired == 1) g_nodeBusyLogged = true;
                    } else if (!g_nodeMissingLogged) {
                        g_nodeMissingLogged = true;
                        Log::Writef("Cutscenes: %s: no level cutscene node plays it (%d cutscene nodes loaded%s%s%s)",
                                    g_playId.c_str(), find.total, g_nodeHooks ? "" : "; node hooks off",
                                    g_playVia.empty() ? "" : "; none for its way there either: ", g_playVia.c_str());
                    }
                }
                // the fired node's stream until it plays (why it waits: state 3 = actors / data not ready)
                if (g_nodeFires && now >= g_nodeReportAt && g_nodeReports < 12) {
                    g_nodeReportAt = now + 2000;
                    ++g_nodeReports;
                    const NodeFind find = FindCineNode(g_fireId.empty() ? g_playId : g_fireId);
                    void* impl = find.node ? NodeImpl(find.node) : nullptr;
                    int id = 0, state = -1, requested = -1, started = -1, done = -1, handler = -1;
                    if (impl && StreamState(impl, id, state, requested, started, done, handler)) {
                        Log::Writef("Cutscenes: %s: its stream %d: state %d, play requested %d, started %d, done %d, handler %d "
                                    "(node state %d)", g_playId.c_str(), id, state, requested, started, done, handler, find.state);
                        if (state == 3 && handler && g_actorReports < 3) {
                            ++g_actorReports;
                            char report[1536];
                            ActorReport(impl, report, sizeof(report));
                            Log::Writef("Cutscenes:   %s", report);
                        }
                    } else {
                        Log::Writef("Cutscenes: %s: the node's cutscene has no stream (impl %p, node state %d)", g_playId.c_str(),
                                    impl, find.state);
                    }
                }
                // the viewer: no node of ours (nor of its way there) 8 s after the level went quiet and its layers were
                // looked for (8 s after they were turned on): ours started directly in the unit that lists it (Long
                // Journey: av_000_walk_talk lists 13_longjourney_cine; none of its 3 nodes played it in 0.9.360)
                if (g_viewerMode && g_noNodeSince && now - g_noNodeSince >= 8000 && now - g_quietSince >= quietNeeded &&
                    !g_layerUnits.empty() && (!g_layersOn || now - g_layersAt >= 8000)) {
                    int unitId = g_cineUnit.empty() ? 0 : LoadedUnitId(g_cineUnit.c_str());
                    if (!unitId || CineIndex(unitId, g_playId.c_str()) < 0) unitId = LoadedUnitId(g_playUnit.c_str());
                    if (unitId && CineIndex(unitId, g_playId.c_str()) >= 0) {
                        Log::Writef("Cutscenes: %s: no node of it in the level %.0f s after it went quiet; started directly in "
                                    "unit %d", g_playId.c_str(), (now - g_noNodeSince) / 1000.0, unitId);
                        g_directStart = true;
                        if (!g_chainStep) EndLevelMusic("before the viewer starts it");
                        Play(g_playId, unitId);
                        break;
                    }
                    if (!g_directMissLogged) {
                        g_directMissLogged = true;   // (the 45 s limit below ends the wait)
                        Log::Writef("Cutscenes: %s: no node of it, and no loaded unit lists it (%s, %s)", g_playId.c_str(),
                                    g_cineUnit.c_str(), g_playUnit.c_str());
                    }
                }
                // (the test panel only: without a node, ours is started directly in the loaded unit)
                if (!g_viewerMode && g_nodeMissingLogged && now - g_phaseSince >= 6000 && g_lastSeenCine.empty()) {
                    if (const int unitId = LoadedUnitId(g_playUnit.c_str())) {
                        Log::Writef("Cutscenes: no cutscene 6 s after the load; starting %s directly", g_playId.c_str());
                        Play(g_playId, unitId);
                        break;
                    }
                }
                const unsigned limit = g_chainStep ? 20u : (g_viewerMode ? 45u : 120u);   // (s)
                if (now - g_phaseSince > limit * 1000u) {
                    // (the layers then: which of the unit's were off, for cutscenes.json "layers")
                    if (g_viewerMode)
                        LogActiveLayers(LoadedUnitId((g_cineUnit.empty() ? g_playUnit : g_cineUnit).c_str()),
                                        "when it did not come");
                    g_phase = Phase::Idle;
                    g_chainGapSince = 0;
                    SetStatus("%s did not come in %u s after %s (last cutscene: %s)", g_playId.c_str(), limit,
                              g_chainStep ? "the one before it" : "the load",
                              g_lastSeenCine.empty() ? "none" : g_lastSeenCine.c_str());
                    if (g_viewerMode) {
                        g_viewerFailed = g_chainStep == 0;   // (the first one played: no error shown)
                        ReturnToMainMenu();
                    }
                }
                break;
            }
            if (now - g_phaseSince < 4000) break;
            if (const int unitId = LoadedUnitId(g_playUnit.c_str())) Play(g_playId, unitId);
            else {
                g_phase = Phase::Idle;
                SetStatus("%s is not loaded any more", g_playUnit.c_str());
            }
            break;
        }
        case Phase::Playing: {
            const bool playing = CineIsPlaying();
            const int frame = CineFrame();
            g_frame = frame;
            if (playing && !g_seenPlaying) LogBlack(now);
            if (playing) g_seenPlaying = true;
            if (!g_seenPlaying) g_waitReason = Wait::Stream;
            if (!g_seenPlaying && now - g_lastFrameLog >= 1000) {   // (until it plays: its stream, each second)
                g_lastFrameLog = now;
                int id = 0, state = -1, requested = -1, started = -1, done = -1, handler = -1;
                if (StreamState(g_impl, id, state, requested, started, done, handler)) {
                    Log::Writef("Cutscenes: %s stream %d: state %d, play requested %d, started %d, done %d, handler %d, "
                                "current handler %s", g_playId.c_str(), id, state, requested, started, done, handler,
                                AnyCinematicPlaying() ? "playing" : "none playing");
                    static int reports = 0;
                    if (state == 3 && handler && reports < 3) {
                        ++reports;
                        char report[1024];
                        ActorReport(g_impl, report, sizeof(report));
                        Log::Writef("Cutscenes:   %s", report);
                    }
                }
                else
                    Log::Writef("Cutscenes: %s: no stream (ID %d)", g_playId.c_str(), id);
            }
            if (playing) {   // (the viewer's fade-out before its end; its hide list)
                int at = 0, total = 0;
                if (CurrentCineFrames(at, total)) SetSecondsLeft(std::max(0.0f, float(total - at) / kCineFps));
                HideForCutscene(now);
                HideOtherNpcs(now);
                LogInstancesNearLara(now);
                LogMovers(now);
                LogCutsceneMusic(now);
                if (g_viewerMode) LogActiveLayers(LoadedUnitId((g_cineUnit.empty() ? g_playUnit : g_cineUnit).c_str()));
            }
            if (playing && now - g_lastFrameLog >= 5000) {
                g_lastFrameLog = now;
                Log::Writef("Cutscenes: %s playing, frame %d", g_playId.c_str(), frame);
            }
            if (!playing && g_seenPlaying && ChainPending()) {   // (the next of its chain: watched for)
                StopCine();
                AdvanceChain(now, false);
                g_phase = Phase::Watching;
            } else if (!playing && g_seenPlaying) {
                StopCine();
                g_phase = Phase::Idle;
                g_secondsLeft = -1.0f;
                if (g_returnToMenu && ReturnToMainMenu())
                    SetStatus("%s ended (%.1f s); back to the main menu", g_playId.c_str(), (now - g_phaseSince) / 1000.0);
                else
                    SetStatus("%s ended (%.1f s)", g_playId.c_str(), (now - g_phaseSince) / 1000.0);
            } else if (!playing && now - g_phaseSince > 15000) {
                // (left as it is: stopping a stream the game may still wait for hung the loading in 0.9.341)
                g_phase = Phase::Idle;
                g_secondsLeft = -1.0f;
                SetStatus("%s has not reported playing in 15 s (frame %d); no longer watched", g_playId.c_str(), frame);
                if (g_viewerMode) {
                    g_viewerFailed = true;
                    ReturnToMainMenu();
                }
            }
            break;
        }
        case Phase::LevelPlaying: {
            char name[128];
            float frame = 0;
            const bool playing = CurrentCine(name, sizeof(name), &frame);
            const bool ours = playing && (SameCine(name, g_playId) || !name[0]);
            g_frame = int(frame);
            if (ours) {
                HideForCutscene(now);
                HideOtherNpcs(now);
                LogInstancesNearLara(now);
                LogMovers(now);
                LogCutsceneMusic(now);
                if (g_viewerMode) LogActiveLayers(LoadedUnitId((g_cineUnit.empty() ? g_playUnit : g_cineUnit).c_str()));
            }
            if (ours) {
                int at = 0, total = 0;
                if (CurrentCineFrames(at, total)) {
                    SetSecondsLeft(std::max(0.0f, float(total - at) / kCineFps));
                    if (!g_lengthLogged) {
                        g_lengthLogged = true;
                        Log::Writef("Cutscenes: %s has %d frames (%.1f s; its stream says %.1f s)", g_playId.c_str(), total,
                                    total / kCineFps, CurrentCineLength());
                    }
                } else {
                    const float length = CurrentCineLength();
                    SetSecondsLeft(length > 0 ? std::max(0.0f, length - frame / kCineFps) : -1.0f);
                }
            }
            if (ours) {   // a frame that stands 3 s (the game paused, or the stream waits)
                if (frame != g_stallFrame) {
                    g_stallFrame = frame;
                    g_stallSince = now;
                } else if (!g_stallLogged && now - g_stallSince >= 3000) {
                    g_stallLogged = true;
                    Log::Writef("Cutscenes: %s stands at frame %.0f for 3 s (the game %s)", g_playId.c_str(), frame,
                                !Features::PauseAvailable() ? "pause state unknown" : (Features::GamePaused() ? "is paused" : "runs"));
                }
            }
            if (ours && now - g_lastFrameLog >= 10000) {
                g_lastFrameLog = now;
                // (the camera's shakes then: In Through the Out Door shook all through in 0.9.390, its start's count 0)
                std::uint32_t own = 0;
                const int shakes = Features::CameraShakesPlaying(false, &own);
                char shakeText[96] = "";
                if (shakes >= 0) snprintf(shakeText, sizeof(shakeText), "; the camera's shakes %d, its own %u", shakes, own);
                Log::Writef("Cutscenes: %s playing (%.0f s, frame %.0f%s)", g_playId.c_str(), (now - g_phaseSince) / 1000.0,
                            frame, shakeText);
            }
            if (!ours && ChainPending() && (!playing || SameCine(name, g_chain[g_chainStep]))) {
                // the next of its chain: playing already (the play goes on), or watched for
                AdvanceChain(now, playing);
                if (!playing) g_phase = Phase::Watching;
                break;
            }
            if (!ours) {
                g_phase = Phase::Idle;
                g_secondsLeft = -1.0f;
                if (playing) Log::Writef("Cutscenes: the game goes on with %s", name);
                if (g_returnToMenu && ReturnToMainMenu())
                    SetStatus("%s ended (%.1f s); back to the main menu", g_playId.c_str(), (now - g_phaseSince) / 1000.0);
                else
                    SetStatus("%s ended (%.1f s)", g_playId.c_str(), (now - g_phaseSince) / 1000.0);
            }
            break;
        }
    }
}

// A cutscene in a unit of its own ("cine_..." with no other cutscene in the catalog) whose Chapter Select start is
// elsewhere, or that has none: not reachable yet. The Drowning's "PCAP: 30" entry starts at Thin Ice's zipline, long
// before cine_30_drowning streams in; loading such a unit alone by name (0.9.351) never ends its loading (state 4).
// (g_catalogMutex held)
std::string OwnCineUnit(const Entry& e) {
    const std::string unit = Lower(e.unit);
    if (unit.rfind("cine_", 0) != 0 || unit == Lower(e.startUnit) || !e.via.empty()) return {};
    int count = 0;
    for (const Entry& other : g_catalog) count += !other.video && Lower(other.unit) == unit;
    return count == 1 ? e.unit : std::string();
}

// The game's order: Chapter Select lists its story in the order the game plays it (Avalanche, then the Black Sea
// Prologue with the London scenes, Bear Valley, ...), except its "Cinematics" sprint (the PCAP test entries). A
// cutscene goes with the sprint of its start's story entry; a start only the PCAP entries name goes with the story
// entries of its unit whose runnable has the same prefix ("sh1", "bv", "pt", "vh1", "fa"), else with any of its unit.
// Within a sprint: the scene number (as the catalog). Without one: after the rest, by scene number.
std::string RunnablePrefix(const char* runnable) {
    std::string name = Lower(runnable);
    if (const auto slash = name.find_last_of("\\/"); slash != std::string::npos) name = name.substr(slash + 1);
    return name.substr(0, name.find('_'));
}
std::size_t StoryPlace(const Entry& e, const std::vector<Features::UnitInfo>& units) {
    const auto story = [](const Features::UnitInfo& u) { return _stricmp(u.sprintName, "Cinematics") != 0; };
    const auto sprintStart = [&](const Features::UnitInfo& entry) {
        for (std::size_t i = 0; i < units.size(); ++i)
            if (story(units[i]) && std::strcmp(units[i].sprintName, entry.sprintName) == 0 &&
                std::strcmp(units[i].releaseName, entry.releaseName) == 0)
                return i;
        return units.size();
    };
    const std::string prefix = RunnablePrefix(e.startRunnable.c_str());
    const Features::UnitInfo* samePrefix = nullptr;
    const Features::UnitInfo* sameUnit = nullptr;
    for (const auto& u : units) {
        if (!story(u)) continue;
        const char* unit = ZoneList::Name(u.unitId);
        if (!unit || _stricmp(unit, e.startUnit.c_str()) != 0) continue;
        if (_stricmp(u.runnableName, e.startRunnable.c_str()) == 0) return sprintStart(u);
        if (!samePrefix && RunnablePrefix(u.runnableName) == prefix) samePrefix = &u;
        if (!sameUnit) sameUnit = &u;
    }
    if (samePrefix) return sprintStart(*samePrefix);
    if (sameUnit) return sprintStart(*sameUnit);
    return units.size();
}

std::vector<ViewerItem> PlayableItems() {
    std::vector<Features::UnitInfo> units(std::size_t(std::max(0, Features::UnitCount())) + 16);
    units.resize(std::size_t(std::max(0, Features::CopyUnits(units.data(), int(units.size())))));
    std::vector<ViewerItem> items;
    std::vector<std::pair<std::size_t, std::pair<int, std::string>>> order;
    std::lock_guard lock(g_catalogMutex);
    std::set<std::string> chained;   // (played with the one before them)
    for (const Entry& e : g_catalog)
        for (const std::string& next : e.then) chained.insert(Lower(next));
    for (const Entry& e : g_catalog) {
        if (e.video || e.hidden || LeftOutOfViewer(e.id) || chained.count(Lower(e.id))) continue;
        if (!OwnCineUnit(e).empty()) continue;   // (its unit alone never finishes loading: 0.9.351's Execution)
        if (e.startUnit.empty()) continue;
        bool found = false;
        for (const auto& u : units) {
            const char* name = ZoneList::Name(u.unitId);
            if (name && _stricmp(name, e.startUnit.c_str()) == 0 && _stricmp(u.runnableName, e.startRunnable.c_str()) == 0) {
                found = true;
                break;
            }
        }
        if (!found) continue;
        items.push_back({e.id, e.title, e.category, e.description, e.image, e.titles, e.descriptions});
        order.push_back({StoryPlace(e, units), SortKey(e)});
    }
    std::vector<std::size_t> sorted(items.size());
    for (std::size_t i = 0; i < sorted.size(); ++i) sorted[i] = i;
    std::stable_sort(sorted.begin(), sorted.end(), [&](std::size_t a, std::size_t b) { return order[a] < order[b]; });
    std::vector<ViewerItem> result;
    result.reserve(items.size());
    for (const std::size_t i : sorted) result.push_back(std::move(items[i]));
    return result;
}

void ReloadUserText() {
    if (g_scanState.load() != 2) return;
    nlohmann::json doc;
    try {
        std::ifstream in(CatalogPath());
        if (!in) return;
        doc = nlohmann::json::parse(in);
    } catch (...) {
        Log::Write("Cutscenes: cutscenes.json could not be read again (kept what was loaded)");
        return;
    }
    ReadPictureLayout(doc);
    if (!doc.contains("cutscenes") || !doc["cutscenes"].is_array()) return;
    std::map<std::string, const nlohmann::json*> saved;
    for (const auto& item : doc["cutscenes"])
        if (item.is_object() && item.contains("id") && item["id"].is_string()) saved[Lower(item["id"].get<std::string>())] = &item;
    std::lock_guard lock(g_catalogMutex);
    for (Entry& e : g_catalog) {
        const auto it = saved.find(Lower(e.id));
        if (it == saved.end()) continue;
        const nlohmann::json& item = *it->second;
        try {
            e.title = FileTitle(item, e.id, e.title);
            e.category = item.value("category", e.category);
            e.description = item.value("description", e.description);
            ReadTranslations(item, e);
            e.image = item.value("image", e.image);
            e.hidden = item.value("hidden", e.hidden);
            ReadHideList(item, e.hide);
            ReadRemoveList(item, e.id, e.remove);
            ReadLayerList(item, e.id, e.layers);
            ReadLayerList(item, e.id, e.layersOff, "layersOff");
            ReadLayerList(item, e.id, e.layersOffAtStart, "layersOffAtStart");
            ReadLayerList(item, e.id, e.units, "units");
            ReadThenList(item, e.id, e.then);
            ReadFlags(item, e);
            ReadStoryEvent(item, e);
            if (item.contains("start") && item["start"].is_object()) {
                e.startUnit = item["start"].value("unit", e.startUnit);
                e.startRunnable = item["start"].value("runnable", e.startRunnable);
                e.trigger = item["start"].value("trigger", e.trigger);
                e.via = item["start"].value("via", e.via);
            }
        } catch (...) {
        }
    }
}

std::wstring ImagesDirectory() { return (CatalogPath().parent_path() / L"images").wstring(); }

bool CatalogReady() { return g_scanState.load() == 2 && Features::UnitListVersion() != 0; }

bool PlayForViewer(const std::string& id) {
    if (!g_gameReady || g_phase != Phase::Idle) return false;
    std::string unit, runnable, ownUnit, via;
    {
        std::lock_guard lock(g_catalogMutex);
        for (const Entry& e : g_catalog)
            if (e.id == id) {
                unit = e.startUnit;
                runnable = e.startRunnable;
                ownUnit = OwnCineUnit(e);
                via = e.via;
            }
    }
    if (!ownUnit.empty()) return false;
    if (unit.empty()) return false;
    std::vector<Features::UnitInfo> units(std::size_t(std::max(0, Features::UnitCount())) + 16);
    units.resize(std::size_t(std::max(0, Features::CopyUnits(units.data(), int(units.size())))));
    for (const auto& u : units) {
        const char* name = ZoneList::Name(u.unitId);
        if (!name || _stricmp(name, unit.c_str()) != 0 || _stricmp(u.runnableName, runnable.c_str()) != 0) continue;
        g_viewerMode = true;
        g_viewerFailed = false;
        g_secondsLeft = -1.0f;
        g_returnToMenu = true;
        std::lock_guard lock(g_requestMutex);
        g_request = Request{true, false, true, id, unit, u.unitId, u.runnableName, false, via};
        return true;
    }
    return false;
}

Stage CurrentStage() {
    switch (g_phase.load()) {
        case Phase::LoadingUnit:
        case Phase::Settling: return Stage::Loading;
        case Phase::Watching: {   // (between two of a chain: the picture kept for 0.25 s, the next may follow at once)
            const ULONGLONG gap = g_chainGapSince.load();
            return gap && GetTickCount64() - gap < 250 ? Stage::Playing : Stage::PreRoll;
        }
        case Phase::Playing: return g_seenPlaying ? Stage::Playing : Stage::PreRoll;   // (started: the viewer's black
                                                                                      // stays until it plays)
        case Phase::LevelPlaying: return Stage::Playing;
        default: return Stage::Idle;
    }
}

float TargetSecondsLeft() { return g_secondsLeft.load(); }
bool AnyCutscenePlaying() {
    if (!g_gameReady) return false;
    char name[96];
    return CurrentCine(name, sizeof(name), nullptr);
}
bool LastViewerPlayFailed() { return g_viewerFailed.load(); }
std::string ViewerTrigger(const std::string& id) {
    std::lock_guard lock(g_catalogMutex);
    for (const Entry& e : g_catalog)
        if (e.id == id) return e.trigger;
    return {};
}
void HoldNodeFiring(bool hold) { g_nodeHold = hold; }
PictureLayout CurrentPictureLayout() {
    std::lock_guard lock(g_pictureMutex);
    return g_pictureLayout;
}
void EndViewerMode() { g_viewerMode = false; }
void AbortToMainMenu() { g_abortRequested = true; }
void GameStates(int& game, int& net) { ReadGameStates(game, net); }
int StreamUnitSlots(UnitSlot* out, int capacity) {
    if (!g_gameReady) return 0;
    int n = 0;
    __try {
        auto* tracker = reinterpret_cast<std::uint8_t*>(GameBuild::Base() + kRvaStreamTracker);
        for (int i = 0; i < kUnitCount && n < capacity; ++i, ++n) {
            std::uint8_t* unit = tracker + std::size_t(i) * kUnitStride;
            out[n].id = *reinterpret_cast<const int*>(unit);
            out[n].state = unit[4] & 0x7F;
            std::memcpy(out[n].name, unit + kUnitNameOffset, sizeof(out[n].name) - 1);
            out[n].name[sizeof(out[n].name) - 1] = 0;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    return n;
}

void DrawTestPanel() {
    ImGui::TextWrapped("Cutscene viewer test: the catalog (ROTTRFramework\\cutscenes\\cutscenes.json) and playing a "
                       "cutscene in its unit.");
    std::lock_guard lock(g_catalogMutex);
    ImGui::Text("Catalog: %s", g_scanStatus.c_str());
    ImGui::Text("Playing: %s", g_playStatus);
    if (!g_gameReady) ImGui::TextColored(ImVec4(1, 0.5f, 0.4f, 1), "Playing is off (unsupported game build).");
    if (g_phase == Phase::Playing) {
        ImGui::Text("Frame %d", g_frame.load());
        ImGui::SameLine();
        if (ImGui::Button("Stop")) {
            std::lock_guard lock(g_requestMutex);
            g_request.pending = true;
            g_request.stop = true;
        }
    }
    bool back = g_returnToMenu.load();
    if (ImGui::Checkbox("Back to the main menu when the cutscene ends", &back)) g_returnToMenu = back;
    if (g_scanState != 2) return;

    static int category = 0;
    static const char* kCategories[] = {"All", "Story", "Baba Yaga", "Blood Ties", "Cold Darkness"};
    for (int i = 0; i < 5; ++i) {
        if (i) ImGui::SameLine();
        if (ImGui::RadioButton(kCategories[i], category == i)) category = i;
    }
    static int selected = -1;
    if (ImGui::BeginChild("cutscene_list", ImVec2(ImGui::GetContentRegionAvail().x * 0.5f, 0), true)) {
        for (int i = 0; i < int(g_catalog.size()); ++i) {
            const Entry& e = g_catalog[std::size_t(i)];
            if (category && e.category != kCategories[category]) continue;
            char label[256];
            std::snprintf(label, sizeof(label), "%s%s##%d", e.title.c_str(), e.video ? " (video)" : "", i);
            if (ImGui::Selectable(label, selected == i)) selected = i;
        }
    }
    ImGui::EndChild();
    ImGui::SameLine();
    if (ImGui::BeginChild("cutscene_detail", ImVec2(0, 0), true)) {
        if (selected >= 0 && selected < int(g_catalog.size())) {
            const Entry& e = g_catalog[std::size_t(selected)];
            ImGui::TextWrapped("%s", e.title.c_str());
            ImGui::TextDisabled("%s", e.id.c_str());
            ImGui::Text("Category: %s", e.category.c_str());
            if (!e.video) {
                ImGui::Text("Unit: %s", e.unit.empty() ? "(unknown)" : e.unit.c_str());
                ImGui::Text("Archive: %s", e.archive.c_str());
                const bool idle = g_phase == Phase::Idle && g_gameReady && !e.unit.empty();
                // the game's Chapter Select entries
                static std::vector<Features::UnitInfo> units;
                static std::uint64_t unitsVersion = ~0ull;
                if (Features::UnitListVersion() != unitsVersion) {
                    unitsVersion = Features::UnitListVersion();
                    units.resize(std::size_t(std::max(0, Features::UnitCount())) + 16);
                    units.resize(std::size_t(std::max(0, Features::CopyUnits(units.data(), int(units.size())))));
                }
                ImGui::BeginDisabled(!idle);
                if (!e.startUnit.empty()) {
                    ImGui::TextWrapped("Start: %s / %s%s", e.startUnit.c_str(), e.startRunnable.c_str(),
                                       e.startFromFile ? " (cutscenes.json)" : "");
                    const Features::UnitInfo* start = nullptr;
                    for (const auto& u : units) {
                        const char* name = ZoneList::Name(u.unitId);
                        if (name && _stricmp(name, e.startUnit.c_str()) == 0 &&
                            _stricmp(u.runnableName, e.startRunnable.c_str()) == 0) {
                            start = &u;
                            break;
                        }
                    }
                    if (start && ImGui::Button("Play from its start")) {
                        std::lock_guard requestLock(g_requestMutex);
                        g_request = Request{true, false, true, e.id, e.startUnit, start->unitId, start->runnableName};
                    }
                    if (!start) ImGui::TextDisabled("(that Chapter Select entry is not in the game's list)");
                } else {
                    ImGui::TextDisabled("No start known: try the Chapter Select entries of its unit below.");
                }
                if (ImGui::Button("Play (its unit loaded)")) {
                    std::lock_guard requestLock(g_requestMutex);
                    g_request = Request{true, false, false, e.id, e.unit};
                }
                if (ImGui::Button("Load its unit, then play")) {
                    std::lock_guard requestLock(g_requestMutex);
                    g_request = Request{true, false, true, e.id, e.unit};
                }
                // the game's Chapter Select entries of this unit (a story's runnable may start the cutscene itself),
                // folded away: the test only
                int count = 0;
                for (const auto& u : units) {
                    const char* name = ZoneList::Name(u.unitId);
                    count += name && _stricmp(name, e.unit.c_str()) == 0;
                }
                char header[96];
                std::snprintf(header, sizeof(header), "All Chapter Select entries of %s (%d)###csentries", e.unit.c_str(), count);
                const bool open = count && ImGui::TreeNode(header);
                int shown = 0;
                for (const auto& u : units) {
                    if (!open) break;
                    const char* name = ZoneList::Name(u.unitId);
                    if (!name || _stricmp(name, e.unit.c_str()) != 0) continue;
                    char label[400];
                    std::snprintf(label, sizeof(label), "Chapter Select: %s / %s##cs%d", u.storyName[0] ? u.storyName : "?",
                                  u.runnableName[0] ? u.runnableName : "no runnable", shown++);
                    if (ImGui::Button(label)) {
                        std::lock_guard requestLock(g_requestMutex);
                        g_request = Request{true, false, true, e.id, e.unit, u.unitId, u.runnableName};
                    }
                }
                if (open) ImGui::TreePop();
                if (!count) ImGui::TextDisabled("(no Chapter Select entry for this unit)");
                ImGui::EndDisabled();
            } else {
                ImGui::TextDisabled("A pre-rendered video: not playable yet.");
            }
            if (!e.description.empty()) ImGui::TextWrapped("%s", e.description.c_str());
        }
    }
    ImGui::EndChild();
}
}  // namespace Cutscenes
