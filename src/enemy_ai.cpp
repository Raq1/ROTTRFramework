#include "enemy_ai.h"
#include "brawl.h"
#include "dtp_edit.h"
#include "game_build.h"
#include "log.h"
#include <Windows.h>
#include <MinHook.h>
#include <imgui.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <vector>

// Fire tokens for the enemies' ranged attacks (RE notes: re\enemy_ai\README.txt, notes_data.md).
//
// Why: Lara now fights close to the enemies (acrobatics, grapple kick, dual pistols), and in the game every enemy that
// sees her shoots at the same time. The game's attack limit (BattleConductor, dtp::BattleConductorSettings
// maxNumAttackers = 1) only covers the melee style attacks; the ranged attack asks
// BattleConductor::OKToStartAttackType(Range), and Range has no cooldown and no attacker list. The gun soldiers' own
// tune has m_MinTimeBetween 0: a burst, half a second, the next burst, from every one of them.
//
// How: Action_Attack_Ranged::AreWeReadyToFire is the one gate every ranged attack passes (timers, range, line of
// sight, permission); Process_Waiting starts the attack when it says yes and keeps waiting (aiming) when it says no.
// The hook lets the game decide first and, for a yes against the player, asks the token list:
// - at most MaxShooters enemies hold a token at a time (MaxShootersLargeGroup when LargeGroup or more want to shoot);
// - a token ends when the enemy's attack is over (BattleConductor::DoneAttacking, or the enemy asking again, or
//   MaxHoldMs; a shot on the move, RequirementActionUpdate's "mobile" check, after 1.5 s); the enemy then waits NpcCooldownMs before its next token and nobody gets one for GapMs;
// - the enemy in front of Lara is the one that fights her (v0.9.249; in the v0.9.248 test the far ones shot while the
//   one next to her waited for the token): enemies within CloseRange have their own token (one of them at a time, a
//   short cooldown), and while one of them wants to shoot the far enemies wait; a far one still takes a turn every
//   FarTurnMs. Among the enemies asking at the same time the nearest goes first (off camera counts as 15 m farther).
// With the tokens off the list still counts (nothing is refused): the log then shows the game's own behaviour.
//
// Melee tokens (v0.9.250): the game already lets one enemy at a time attack in melee (BattleConductor: the target's
// attacker list, dtp::BattleConductorSettings maxNumAttackers = 1; the others wait), but the soldiers with guns have
// m_BattleConductorPermissionNotRequired set in their melee tune (dtp::NpcComponent +0x9E0 bit 4): next to Lara every
// one of them hits without asking. Action_Attack_Melee::IsViableForPlan hands that bit to
// Action_Attack_Base::BasicAttack_IsViableForPlan, which otherwise asks CheckAttacker / IsTargetAvailable /
// OKToStartAttackType(Melee). With the melee tokens on, the bit is cleared in the tune of every enemy in the level (the
// tune is the enemy type's data, shared by its instances; the bit goes back when the option is turned off), and
// maxNumAttackers is set to "enemies in melee at once".
//
// State control guard (v0.9.248): the v0.9.247 test crashed when an enemy entered its finisher victim state: the entry
// function of activity 282 (Physics_IncreaseProxyRankForThisState) in that enemy's SCAgent table (m_enumData, 0x80 per
// enum value, the functions at +8..+0x48) had lost the upper half of its address (0x04F46A00 for ROTTR+0x686A00). What
// wrote there is not known. Every tick the guard reads the function slots of every enemy's agents, logs a value that
// is not a function of the game (or of this framework), and puts a truncated address back.
// Marvel's Avengers (same engine) has this built in: exclusive attacks with a cooldown, on screen time as an input.
namespace EnemyAI {
namespace {
constexpr std::uintptr_t kRvaAreWeReadyToFire = 0x004BEC90ull;   // Action_Attack_Ranged::AreWeReadyToFire(npc, target, mobile, reason)
constexpr std::uintptr_t kRvaGameTimeUse = 0x004BECC2ull;        // mov eax,[game time ms]; cmp eax,[rdx+17ECh]
constexpr std::uintptr_t kRvaGameTime = 0x02D5796Cull;
constexpr std::uintptr_t kRvaDoneAttacking = 0x004D4FA0ull;      // BattleConductor::DoneAttacking(attacker, target, type)
constexpr std::uintptr_t kRvaNpcInstanceUse = 0x004CA7F2ull;     // mov rdx,[rsi+3C8h] (Action_Attack_Base::CheckAttackPermission)
constexpr std::uintptr_t kRvaNpcOnCameraUse = 0x004BF49Aull;     // mov ecx,[rbx+179Ch] (NpcComponent::AssaultMode_Valid)
constexpr std::uintptr_t kRvaInstanceSlots = 0x024EEAD0ull;
constexpr std::uintptr_t kRvaTryToGetStateControl = 0x00218BE0ull; // cdc::TryToGetStateControlComponent(instance handle)
constexpr std::uintptr_t kRvaGetAgent = 0x006D9540ull;             // StateControlComponent::GetAgent(index)
constexpr std::uintptr_t kRvaNpcListLoad = 0x0070259Full;          // mov rbx,[NpcComponent list head]
constexpr std::uintptr_t kRvaNpcList = 0x02D62CE8ull;
constexpr std::uintptr_t kRvaNpcListNextUse = 0x007025CFull;       // mov rbx,[rbx+1A58h]
constexpr std::uintptr_t kRvaNpcTuneUse = 0x004ADE41ull;           // mov r14,[rax+3C0h] (Action_Attack_Melee's constructor)
constexpr std::uintptr_t kRvaMeleeTuneUse = 0x004ADE57ull;         // mov ecx,[r14+9E0h]
constexpr std::uintptr_t kRvaMeleePermissionUse = 0x004A97E7ull;   // shr eax,4; and al,1 (Action_Attack_Melee::IsViableForPlan)
constexpr std::uintptr_t kRvaConductorUse = 0x004CA6BBull;         // lea rcx,[BattleConductor::s_BattleConductor]
constexpr std::uintptr_t kRvaConductor = 0x02D5B250ull;
constexpr std::uintptr_t kRvaGetLocalPlayerInstance = 0x00403BF0ull;
constexpr std::uintptr_t kRvaNpcAlertLevelUse = 0x004BF533ull;     // cmp dword ptr [rbx+18F0h],3 (NpcComponent::AssaultMode_Valid)
constexpr std::uintptr_t kRvaNpcDeadUse = 0x004F28D7ull;           // test byte ptr [rcx+1931h],1

constexpr std::uint8_t kAreWeReadyToFireSig[] = {0x48, 0x89, 0x5C, 0x24, 0x20, 0x56, 0x57, 0x41, 0x57, 0x48, 0x83, 0xEC, 0x60,
                                                 0x45, 0x0F, 0xB6, 0xF9, 0x49, 0x8B, 0xF8, 0x48, 0x8B, 0xDA, 0x48, 0x8B, 0xF1};
constexpr std::uint8_t kGameTimeUseSig[] = {0x8B, 0x05};
constexpr std::uint8_t kGameTimeUseTailSig[] = {0x3B, 0x82, 0xEC, 0x17, 0x00, 0x00};
constexpr std::uint8_t kDoneAttackingSig[] = {0x48, 0x89, 0x5C, 0x24, 0x10, 0x48, 0x89, 0x6C, 0x24, 0x18, 0x48, 0x89, 0x74, 0x24, 0x20,
                                              0x57, 0x41, 0x56, 0x41, 0x57, 0x45, 0x33, 0xFF, 0x49, 0x63, 0xD9};
constexpr std::uint8_t kNpcInstanceUseSig[] = {0x48, 0x8B, 0x96, 0xC8, 0x03, 0x00, 0x00};
constexpr std::uint8_t kNpcOnCameraUseSig[] = {0x8B, 0x8B, 0x9C, 0x17, 0x00, 0x00};
constexpr std::uint8_t kTryToGetStateControlSig[] = {0x48, 0xB8, 0x00, 0x00, 0x00, 0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0x48, 0x85, 0xC8, 0x74, 0x44};
constexpr std::uint8_t kGetAgentSig[] = {0x48, 0x8B, 0x81, 0xA8, 0x02, 0x00, 0x00, 0x4C, 0x8B, 0xC1, 0x3B, 0x50, 0x08, 0x72, 0x03, 0x33, 0xC0, 0xC3};
constexpr std::uint8_t kNpcListLoadSig[] = {0x48, 0x8B, 0x1D};
constexpr std::uint8_t kNpcListNextUseSig[] = {0x48, 0x8B, 0x9B, 0x58, 0x1A, 0x00, 0x00};
constexpr std::uint8_t kNpcTuneUseSig[] = {0x4C, 0x8B, 0xB0, 0xC0, 0x03, 0x00, 0x00};
constexpr std::uint8_t kMeleeTuneUseSig[] = {0x41, 0x8B, 0x8E, 0xE0, 0x09, 0x00, 0x00};
constexpr std::uint8_t kMeleePermissionUseSig[] = {0xC1, 0xE8, 0x04, 0x24, 0x01};
constexpr std::uint8_t kConductorUseSig[] = {0x48, 0x8D, 0x0D};
constexpr std::uint8_t kGetLocalPlayerInstanceSig[] = {0x40, 0x53, 0x48, 0x83, 0xEC, 0x20, 0x0F, 0xB6, 0xD9};
constexpr std::uint8_t kNpcAlertLevelUseSig[] = {0x83, 0xBB, 0xF0, 0x18, 0x00, 0x00, 0x03};
constexpr std::uint8_t kNpcDeadUseSig[] = {0xF6, 0x81, 0x31, 0x19, 0x00, 0x00, 0x01};

constexpr std::size_t kNpcInstance = 0x3C8;       // NpcComponent: its Instance*
constexpr std::size_t kNpcOnCameraSince = 0x179C; // NpcComponent: game time it came on camera (0 = off camera)
constexpr std::size_t kInstancePosition = 0x10;
constexpr std::size_t kInstanceFlags = 0xC8;      // bit 5: a player
constexpr std::size_t kInstanceHandle = 0x118;
constexpr std::size_t kInstanceSlotSize = 0x120;
constexpr std::uint32_t kInstanceSlotCount = 30000;
constexpr std::size_t kNpcTune = 0x3C0;           // NpcComponent: its dtp::NpcComponent (the enemy type's tuning)
constexpr std::size_t kTuneMeleeFlags = 0x9E0;    // dtp::NpcComponent::MeleeAttackTune: the flag bits
constexpr std::uint32_t kMeleeNoPermission = 0x10; // m_BattleConductorPermissionNotRequired
constexpr std::size_t kConductorSettings = 0x10;  // BattleConductor::m_pSettings; maxNumAttackers is its first int
constexpr int kMaxMeleeTunes = 64;
constexpr std::size_t kNpcAlertLevel = 0x18F0, kNpcDead = 0x1931;
constexpr std::size_t kNpcNext = 0x1A58;          // NpcComponent: the next one in the manager's list
constexpr std::size_t kComponentAgentInfo = 0x2A8; // StateControlComponent: its dtp, the agent count at +8
constexpr std::size_t kAgentEnumData = 0x1C40;    // SCAgent::m_enumData: SCEnumDataHolder[878], 0x80 each
constexpr std::size_t kEnumEntrySize = 0x80;
constexpr int kEnumEntries = 878;
constexpr std::size_t kEnumFirstFunction = 0x08;  // condition, property, then the seven activity functions
constexpr std::size_t kEnumLastFunction = 0x40;   // +0x48 holds data in the retail build (v0.9.248 log: floats, heap pointers)
constexpr int kMaxGuardNpcs = 64;
constexpr std::uint32_t kMaxGuardAgents = 4;

using AreWeReadyToFireFn = bool(__fastcall*)(void* action, std::uint8_t* npc, std::uint8_t* target, bool mobile, char* reason);
using DoneAttackingFn = void(__fastcall*)(void* conductor, std::uint8_t* attacker, std::uint8_t* target, int attackType);
using TryToGetStateControlFn = std::uint8_t*(__fastcall*)(std::uint64_t handle);
using GetAgentFn = std::uint8_t*(__fastcall*)(std::uint8_t* component, std::uint32_t index);
using GetLocalPlayerInstanceFn = std::uint8_t*(__fastcall*)(std::uint8_t);

std::uintptr_t g_base = 0;
bool g_available = false;
AreWeReadyToFireFn g_origAreWeReadyToFire = nullptr;
DoneAttackingFn g_origDoneAttacking = nullptr;
bool g_guardAvailable = false;
TryToGetStateControlFn g_tryToGetStateControl = nullptr;
GetAgentFn g_getAgent = nullptr;
std::uintptr_t g_imageEnd = 0, g_ownBase = 0, g_ownEnd = 0;
std::atomic<bool> g_guard{true};
std::atomic<unsigned> g_guardFound{0};
std::atomic<unsigned> g_guardRepaired{0};
std::atomic<int> g_guardLogBudget{80};
bool g_meleeAvailable = false;
// v0.9.269, melee calm: while Lara fights hand to hand (Brawl::InMelee: a brawl strike or the game's own melee) the
// enemies shoot much less: the far ones not at all, the enemy she is hitting not at all, the other near ones one at a
// time with MeleeCalmGapMs between two tokens and short bursts; and between two melee attacks of the enemies on her
// the BattleConductor waits MeleeCalmMeleeGap seconds (its melee attack type's minTimeBetween, 0 in the game).
std::atomic<bool> g_meleeCalm{false};   // (v1.0.0: off by default)
std::atomic<int> g_meleeCalmGapMs{3000};
std::atomic<int> g_meleeCalmHoldMs{1000};
std::atomic<int> g_meleeCalmMeleeGap{4};   // seconds
std::atomic<unsigned> g_meleeCalmRefusals{0};
std::uint32_t g_lastCalmGrantMs = 0;
float* g_meleeMinTime = nullptr;          // the settings' melee attack entry: minTimeBetween
float g_meleeMinTimeOriginal = 0.0f;
std::atomic<bool> g_meleeInBrawl{false};  // v0.9.256: the tokens are on while a hand-to-hand combo runs
std::atomic<bool> g_meleeTokens{false};  // off by default since v0.9.253: the v0.9.252 test had enemies that ignored Lara
std::atomic<int> g_maxMelee{1};
std::atomic<int> g_meleeLogBudget{60};
std::atomic<int> g_shownMeleeTunes{0};
std::uint8_t* g_meleeTunes[kMaxMeleeTunes]{};   // tunes whose "no permission" bit this module cleared
int g_meleeTuneCount = 0;
const std::uint8_t* g_settingsSeen = nullptr;   // the settings whose maxNumAttackers was read
int g_settingsOriginal = 0;
const std::uint8_t* g_settingsSeenMelee = nullptr;

// The battle settings guard (Modding Suite 2.0.26): BattleConductor::m_pSettings points into a DTP's data, the
// default one (globalInfo's battle conductor settings, by id) or one an action graph node chose
// (AGBattleConductor_ChangeSettingsNode -> BattleConductor::SetDtpSettings, flag 4 at +0x6106). When that DTP is
// unloaded the game keeps the pointer: a crash report had the enemies' melee check (CheckMeleeIsActive ->
// PlayerCombatStatus::GetEnemy -> GameTeam::TeamsAreEnemies, its team list at +0xF0/+0xF8) read the freed memory
// after an outfit swap had loaded new data over it. Each tick: the DTP the settings are in must still be in use, else
// the conductor goes back to its default settings (SetDtpSettings(null): the game's own path), and our own melee
// writes (maxNumAttackers, minTimeBetween) only go to settings found in a loaded DTP.
constexpr std::uintptr_t kRvaSetDtpSettings = 0x00525D30ull;  // BattleConductor::SetDtpSettings(this, settings)
constexpr std::size_t kConductorSettingsFlags = 0x6106;
constexpr std::uint8_t kConductorCustomSettings = 0x04;
constexpr std::uint8_t kSetDtpSettingsSig[] = {
    0x48, 0x89, 0x5C, 0x24, 0x08, 0x57, 0x48, 0x83, 0xEC, 0x20,  // mov [rsp+8], rbx; push rdi; sub rsp, 20h
    0x48, 0x8B, 0x79, 0x10, 0x48, 0x8B, 0xD9,                    // mov rdi, [rcx+10h]; mov rbx, rcx
    0x48, 0x85, 0xD2, 0x74, 0x0D,                                // test rdx, rdx; je
    0x80, 0x89, 0x06, 0x61, 0x00, 0x00, 0x04,                    // or byte ptr [rcx+6106h], 4
    0x48, 0x89, 0x51, 0x10,                                      // mov [rcx+10h], rdx
};
using SetDtpSettingsFn = void(__fastcall*)(void* conductor, void* settings);
SetDtpSettingsFn g_setDtpSettings = nullptr;
bool g_settingsGuardAvailable = false;
const std::uint8_t* g_guardSettings = nullptr;  // the settings pointer last checked
std::uint32_t g_guardSettingsDtp = 0;            // the DTP it is in (0: not found in a loaded DTP)
const void* g_guardSettingsData = nullptr;
bool g_settingsSafe = false;                     // the settings are in a loaded DTP: our melee writes may go there
std::atomic<unsigned> g_settingsReset{0};
std::atomic<int> g_settingsLogBudget{20};
std::atomic<int> g_checkLogBudget{200};

// Settings ([EnemyAI] in ROTTRFramework.ini).
std::atomic<bool> g_fireTokens{false};   // (v1.0.0: off by default)
std::atomic<int> g_maxShooters{1};
std::atomic<int> g_largeGroup{5};
std::atomic<int> g_maxShootersLargeGroup{2};
std::atomic<int> g_gapMs{600};
std::atomic<int> g_npcCooldownMs{2500};
std::atomic<int> g_maxHoldMs{6000};
std::atomic<bool> g_preferOnCamera{true};
std::atomic<int> g_closeRange{700};        // cm: nearer than this = "in front of Lara"
std::atomic<int> g_closeCooldownMs{800};
std::atomic<int> g_farTurnMs{5000};        // 0: no far shots while a near enemy fights
std::atomic<bool> g_log{true};

constexpr std::uint32_t kCloseHoldMs = 3000;   // a near enemy's token (its attack may end without "attack done": it switches to melee)
constexpr float kOffCameraPenalty = 1500.0f;
constexpr float kNearerMargin = 100.0f;
constexpr std::uint32_t kMobileHoldMs = 1500;  // a shot on the move (no "attack done" follows it)
constexpr std::uint32_t kSameAttackMs = 400;   // a holder asking again this soon is still in the same attack
constexpr std::uint32_t kAskingNowMs = 400;    // "asking at the same time"
constexpr std::uint32_t kWantsToShootMs = 1500;
constexpr std::uint32_t kForgetMs = 10000;
constexpr ULONGLONG kSummaryEveryMs = 5000;

enum Refusal { kLimit, kGap, kOwnCooldown, kNearFights, kNearerFirst, kLaraInMelee, kRefusalCount };
constexpr const char* kRefusalNames[kRefusalCount] = {"limit", "gap", "own cooldown", "a near enemy fights Lara", "a nearer enemy first",
                                                      "Lara fights hand to hand"};

struct Shooter {
    std::uint64_t handle = 0;
    std::uint32_t lastAskMs = 0;       // the game last found it ready to fire
    std::uint32_t lastCheckMs = 0;     // its last ranged attack check, ready or not
    std::uint32_t grantedMs = 0;
    std::uint32_t lastTokenEndMs = 0;
    std::uint32_t cooldownUntilMs = 0;
    std::uint32_t holdMs = 0;          // the longest this token may last
    float distance = 0.0f;
    bool onCamera = false;
    bool holding = false;
    bool close = false;                // within CloseRange at its last ready check
    bool checkLogged = false;          // its first ranged attack check is in the log
    std::uint32_t refusalLoggedMs = 0;
};

std::mutex g_mutex;
std::vector<Shooter> g_shooters;
std::uint32_t g_nextGrantMs = 0;       // far enemies
std::uint32_t g_nextCloseGrantMs = 0;  // near enemies
std::uint32_t g_lastFarTurnMs = 0;     // the last far shot taken while a near enemy wanted to shoot
std::uint32_t g_lastNowMs = 0;

// Counters of the running summary period, and what the menu shows.
struct Period {
    unsigned grants = 0;
    unsigned refusals[kRefusalCount]{};
    unsigned mostAtOnce = 0;
    unsigned mostWanting = 0;
};
Period g_period;
ULONGLONG g_periodStart = 0;
std::atomic<int> g_shownHolding{0};
std::atomic<int> g_shownWanting{0};
std::atomic<unsigned> g_totalGrants{0};
std::atomic<unsigned> g_totalRefusals{0};
std::atomic<int> g_grantLogBudget{600};
std::atomic<int> g_summaryLogBudget{400};

template <typename T>
T At(std::uintptr_t rva) {
    return reinterpret_cast<T>(g_base + rva);
}

std::uintptr_t RelTarget(std::uintptr_t rva, std::size_t operandOffset, std::size_t length) {
    const std::int32_t rel = *reinterpret_cast<const std::int32_t*>(g_base + rva + operandOffset);
    return static_cast<std::uintptr_t>(static_cast<std::intptr_t>(rva + length) + rel);
}

bool Verify(std::uintptr_t rva, const std::uint8_t* sig, std::size_t size, const char* name) {
    if (GameBuild::VerifyBytes(rva, sig, size)) return true;
    Log::Writef("EnemyAI: %s signature mismatch; enemy fire tokens unavailable", name);
    return false;
}

// ---------- settings ----------
const char* IniPath() {
    static char path[MAX_PATH]{};
    if (path[0]) return path;
    GetModuleFileNameA(nullptr, path, MAX_PATH);
    char* slash = path;
    for (char* p = path; *p; ++p)
        if (*p == '\\' || *p == '/') slash = p + 1;
    strcpy_s(slash, static_cast<std::size_t>(path + sizeof(path) - slash), "ROTTRFramework.ini");
    return path;
}

constexpr const char* kIniSection = "EnemyAI";

int IniInt(const char* key, int fallback) {
    return static_cast<int>(GetPrivateProfileIntA(kIniSection, key, fallback, IniPath()));
}

void IniWriteInt(const char* key, int value) {
    char buf[32]{};
    std::snprintf(buf, sizeof(buf), "%d", value);
    WritePrivateProfileStringA(kIniSection, key, buf, IniPath());
}

void SaveSettings() {
    IniWriteInt("FireTokens", g_fireTokens.load() ? 1 : 0);
    IniWriteInt("MaxShooters", g_maxShooters.load());
    IniWriteInt("LargeGroup", g_largeGroup.load());
    IniWriteInt("MaxShootersLargeGroup", g_maxShootersLargeGroup.load());
    IniWriteInt("GapMs", g_gapMs.load());
    IniWriteInt("NpcCooldownMs", g_npcCooldownMs.load());
    IniWriteInt("MaxHoldMs", g_maxHoldMs.load());
    IniWriteInt("PreferOnCamera", g_preferOnCamera.load() ? 1 : 0);
    IniWriteInt("CloseRange", g_closeRange.load());
    IniWriteInt("CloseCooldownMs", g_closeCooldownMs.load());
    IniWriteInt("FarTurnMs", g_farTurnMs.load());
    IniWriteInt("Log", g_log.load() ? 1 : 0);
    IniWriteInt("StateControlGuard", g_guard.load() ? 1 : 0);
    IniWriteInt("MeleeTokens", g_meleeTokens.load() ? 1 : 0);
    IniWriteInt("MeleeCalm", g_meleeCalm.load() ? 1 : 0);
    IniWriteInt("MeleeCalmGapMs", g_meleeCalmGapMs.load());
    IniWriteInt("MeleeCalmHoldMs", g_meleeCalmHoldMs.load());
    IniWriteInt("MeleeCalmMeleeGap", g_meleeCalmMeleeGap.load());
    IniWriteInt("MeleeTokensInBrawl", g_meleeInBrawl.load() ? 1 : 0);
    IniWriteInt("MaxMeleeAttackers", g_maxMelee.load());
}

void LoadSettings() {
    g_fireTokens.store(IniInt("FireTokens", 0) != 0);   // (v1.0.0: the features default off)
    g_maxShooters.store(std::clamp(IniInt("MaxShooters", 1), 1, 6));
    g_largeGroup.store(std::clamp(IniInt("LargeGroup", 5), 2, 20));
    g_maxShootersLargeGroup.store(std::clamp(IniInt("MaxShootersLargeGroup", 2), 1, 6));
    g_gapMs.store(std::clamp(IniInt("GapMs", 600), 0, 5000));
    g_npcCooldownMs.store(std::clamp(IniInt("NpcCooldownMs", 2500), 0, 15000));
    g_maxHoldMs.store(std::clamp(IniInt("MaxHoldMs", 6000), 1000, 20000));
    g_preferOnCamera.store(IniInt("PreferOnCamera", 1) != 0);
    g_closeRange.store(std::clamp(IniInt("CloseRange", 700), 150, 3000));
    g_closeCooldownMs.store(std::clamp(IniInt("CloseCooldownMs", 800), 0, 10000));
    g_farTurnMs.store(std::clamp(IniInt("FarTurnMs", 5000), 0, 30000));
    g_log.store(IniInt("Log", 1) != 0);
    g_guard.store(IniInt("StateControlGuard", 1) != 0);
    g_meleeTokens.store(IniInt("MeleeTokens", 0) != 0);
    g_meleeCalm.store(IniInt("MeleeCalm", 0) != 0);
    g_meleeCalmGapMs.store(std::clamp(IniInt("MeleeCalmGapMs", 3000), 0, 20000));
    g_meleeCalmHoldMs.store(std::clamp(IniInt("MeleeCalmHoldMs", 1000), 300, 6000));
    g_meleeCalmMeleeGap.store(std::clamp(IniInt("MeleeCalmMeleeGap", 4), 0, 20));
    g_meleeInBrawl.store(IniInt("MeleeTokensInBrawl", 0) != 0);
    g_maxMelee.store(std::clamp(IniInt("MaxMeleeAttackers", 1), 1, 4));
}

// ---------- the token list (g_mutex held) ----------
std::uint32_t GameTimeMs() {
    return *At<const std::uint32_t*>(kRvaGameTime);
}

bool HandleAlive(std::uint64_t handle) {
    const std::uint32_t index = static_cast<std::uint32_t>(handle);
    if (!handle || index >= kInstanceSlotCount) return false;
    const std::uint8_t* slot = At<const std::uint8_t*>(kRvaInstanceSlots) + static_cast<std::size_t>(index) * kInstanceSlotSize;
    return *reinterpret_cast<const std::uint64_t*>(slot + kInstanceHandle) == handle;
}

void EndToken(Shooter& s, std::uint32_t now, const char* why) {
    if (!s.holding) return;
    s.holding = false;
    s.lastTokenEndMs = now;
    const std::uint32_t gap = static_cast<std::uint32_t>(g_gapMs.load());
    if (s.close) {
        s.cooldownUntilMs = now + static_cast<std::uint32_t>(g_closeCooldownMs.load());
        g_nextCloseGrantMs = (std::max)(g_nextCloseGrantMs, now + gap);
    } else {
        s.cooldownUntilMs = now + static_cast<std::uint32_t>(g_npcCooldownMs.load());
        g_nextGrantMs = (std::max)(g_nextGrantMs, now + gap);
    }
    if (g_log.load() && g_grantLogBudget.fetch_sub(1) > 0)
        Log::Detailf("EnemyAI: %s enemy %u gave its fire token back after %u ms (%s)", s.close ? "near" : "far",
                    static_cast<unsigned>(s.handle), static_cast<unsigned>(now - s.grantedMs), why);
}

// Drops what is stale and counts who holds a token and who wants to shoot.
void Refresh(std::uint32_t now, unsigned& holding, unsigned& wanting) {
    if (now < g_lastNowMs) {  // the game clock went back (a load): start over
        g_shooters.clear();
        g_nextGrantMs = g_nextCloseGrantMs = g_lastFarTurnMs = 0;
    }
    g_lastNowMs = now;
    holding = wanting = 0;
    for (std::size_t i = 0; i < g_shooters.size();) {
        Shooter& s = g_shooters[i];
        if (s.holding && !HandleAlive(s.handle)) EndToken(s, now, "the enemy is gone");
        if (s.holding && now - s.grantedMs > s.holdMs) EndToken(s, now, "time limit");
        if (!s.holding && now - s.lastCheckMs > kForgetMs) {
            s = g_shooters.back();
            g_shooters.pop_back();
            continue;
        }
        if (s.holding) ++holding;
        if (s.holding || now - s.lastAskMs <= kWantsToShootMs) ++wanting;
        ++i;
    }
}

Shooter& FindOrAdd(std::uint64_t handle) {
    for (Shooter& s : g_shooters)
        if (s.handle == handle) return s;
    Shooter s;
    s.handle = handle;
    g_shooters.push_back(s);
    return g_shooters.back();
}

void LogFirstCheck(Shooter& s, bool ready, bool mobile) {
    if (s.checkLogged) return;
    s.checkLogged = true;
    if (g_log.load() && g_checkLogBudget.fetch_sub(1) > 0)
        Log::Detailf("EnemyAI: enemy %u checks its ranged attack on Lara for the first time (%s%s)", static_cast<unsigned>(s.handle),
                    ready ? "ready to fire" : "not ready", mobile ? ", on the move" : "");
}

// An enemy's ranged attack check on the player that the game itself answered with no.
void NoteCheck(std::uint8_t* npc, bool mobile) {
    const std::uint8_t* instance = *reinterpret_cast<std::uint8_t* const*>(npc + kNpcInstance);
    if (!instance) return;
    const std::uint64_t handle = *reinterpret_cast<const std::uint64_t*>(instance + kInstanceHandle);
    if (!handle) return;
    std::lock_guard<std::mutex> lock(g_mutex);
    Shooter& s = FindOrAdd(handle);
    s.lastCheckMs = GameTimeMs();
    LogFirstCheck(s, false, mobile);
}

// The game found this enemy ready to fire at the player: may it? (Always yes with the tokens off; the list still counts.)
bool MayFire(std::uint8_t* npc, std::uint8_t* target, bool mobile) {
    const std::uint8_t* instance = *reinterpret_cast<std::uint8_t* const*>(npc + kNpcInstance);
    if (!instance) return true;
    const std::uint64_t handle = *reinterpret_cast<const std::uint64_t*>(instance + kInstanceHandle);
    if (!handle) return true;
    const float* from = reinterpret_cast<const float*>(instance + kInstancePosition);
    const float* to = reinterpret_cast<const float*>(target + kInstancePosition);
    const float dx = from[0] - to[0], dy = from[1] - to[1], dz = from[2] - to[2];
    const bool restrict = g_fireTokens.load();

    std::lock_guard<std::mutex> lock(g_mutex);
    const std::uint32_t now = GameTimeMs();
    unsigned holding = 0, wanting = 0;
    Refresh(now, holding, wanting);
    Shooter& s = FindOrAdd(handle);
    if (s.holding) {
        if (now - s.grantedMs < kSameAttackMs) return true;
        EndToken(s, now, "its attack is over, it asks again");  // back in its waiting state
        --holding;
    } else if (now - s.lastAskMs > kWantsToShootMs) {
        ++wanting;
    }
    s.lastAskMs = s.lastCheckMs = now;
    s.distance = std::sqrt(dx * dx + dy * dy + dz * dz);
    s.onCamera = *reinterpret_cast<const std::uint32_t*>(npc + kNpcOnCameraSince) != 0;
    s.close = s.distance <= static_cast<float>(g_closeRange.load());

    // Who else holds a token, and is a near enemy in the fight (holding, or asking within the last 1.5 s)?
    unsigned holdingClose = 0, holdingFar = 0;
    bool nearFights = false;
    for (const Shooter& o : g_shooters) {
        if (&o == &s) continue;
        if (o.holding) ++(o.close ? holdingClose : holdingFar);
        if (o.close && (o.holding || now - o.lastAskMs <= kWantsToShootMs)) nearFights = true;
    }
    const bool preferOnCamera = g_preferOnCamera.load();
    const auto rank = [preferOnCamera](const Shooter& x) { return x.distance + (preferOnCamera && !x.onCamera ? kOffCameraPenalty : 0.0f); };

    int refusal = -1;
    bool farTurn = false;
    // Lara fights hand to hand: no shots from far, none from the enemy she is hitting, the other near ones rarely.
    const bool calm = restrict && g_meleeCalm.load() && Brawl::InMelee();
    if (calm) {
        const std::uint32_t calmGap = static_cast<std::uint32_t>(g_meleeCalmGapMs.load());
        if (!s.close || instance == Brawl::LockedEnemy() || (g_lastCalmGrantMs && now - g_lastCalmGrantMs < calmGap)) {
            refusal = kLaraInMelee;
            g_meleeCalmRefusals.fetch_add(1);
        }
    }
    if (restrict && refusal < 0) {
        if (s.close) {
            if (holdingClose >= 1) refusal = kLimit;
            else if (now < g_nextCloseGrantMs) refusal = kGap;
        } else {
            const int farTurnMs = g_farTurnMs.load();
            farTurn = nearFights && farTurnMs > 0 && now - g_lastFarTurnMs >= static_cast<std::uint32_t>(farTurnMs);
            const unsigned limit = static_cast<unsigned>(wanting >= static_cast<unsigned>(g_largeGroup.load())
                                                             ? (std::max)(g_maxShootersLargeGroup.load(), g_maxShooters.load())
                                                             : g_maxShooters.load());
            if (nearFights && !farTurn) refusal = kNearFights;
            else if (holdingFar >= limit) refusal = kLimit;
            else if (now < g_nextGrantMs) refusal = kGap;
        }
        if (refusal < 0 && now < s.cooldownUntilMs) refusal = kOwnCooldown;
        if (refusal < 0) {
            // among the enemies of its kind (near / far) that ask now and could take the token: the nearest first
            for (const Shooter& o : g_shooters) {
                if (&o == &s || o.holding || o.close != s.close || now - o.lastAskMs > kAskingNowMs || now < o.cooldownUntilMs) continue;
                if (rank(o) + kNearerMargin < rank(s)) {
                    refusal = kNearerFirst;
                    break;
                }
            }
        }
    }
    g_period.mostWanting = (std::max)(g_period.mostWanting, wanting);
    g_shownWanting.store(static_cast<int>(wanting));
    LogFirstCheck(s, true, mobile);
    if (refusal >= 0) {
        ++g_period.refusals[refusal];
        g_totalRefusals.fetch_add(1);
        g_shownHolding.store(static_cast<int>(holding));
        if ((!s.refusalLoggedMs || now - s.refusalLoggedMs > 3000) && g_log.load() && g_checkLogBudget.fetch_sub(1) > 0) {
            s.refusalLoggedMs = now;
            Log::Detailf("EnemyAI: %s enemy %u at %.0f cm may not shoot yet (%s); shooting now %u near + %u far, wanting to shoot %u",
                        s.close ? "near" : "far", static_cast<unsigned>(handle), s.distance, kRefusalNames[refusal], holdingClose,
                        holdingFar, wanting);
        }
        return false;
    }
    s.holding = true;
    s.grantedMs = now;
    s.holdMs = mobile ? kMobileHoldMs : (std::min)(static_cast<std::uint32_t>(g_maxHoldMs.load()), s.close ? kCloseHoldMs : 0xFFFFFFFFu);
    if (calm) {
        s.holdMs = (std::min)(s.holdMs, static_cast<std::uint32_t>(g_meleeCalmHoldMs.load()));
        g_lastCalmGrantMs = now;
    }
    ++holding;
    if (restrict) {
        const std::uint32_t gap = static_cast<std::uint32_t>(g_gapMs.load());
        if (s.close) g_nextCloseGrantMs = (std::max)(g_nextCloseGrantMs, now + gap);
        else g_nextGrantMs = (std::max)(g_nextGrantMs, now + gap);
        if (farTurn) g_lastFarTurnMs = now;
    }
    ++g_period.grants;
    g_period.mostAtOnce = (std::max)(g_period.mostAtOnce, holding);
    g_totalGrants.fetch_add(1);
    g_shownHolding.store(static_cast<int>(holding));
    if (g_log.load() && g_grantLogBudget.fetch_sub(1) > 0)
        Log::Detailf("EnemyAI: %s %s enemy %u at %.0f cm, %s%s%s; shooting now %u, wanting to shoot %u", restrict ? "fire token to" : "shot by",
                    s.close ? "near" : "far", static_cast<unsigned>(handle), s.distance, s.onCamera ? "on camera" : "off camera",
                    mobile ? ", on the move" : "", farTurn ? ", a far enemy's turn while a near one fights" : "", holding, wanting);
    return true;
}

bool __fastcall HookAreWeReadyToFire(void* action, std::uint8_t* npc, std::uint8_t* target, bool mobile, char* reason) {
    const bool ready = g_origAreWeReadyToFire(action, npc, target, mobile, reason);
    if (!npc || !target) return ready;
    if (!((*reinterpret_cast<const std::uint32_t*>(target + kInstanceFlags) >> 5) & 1)) return ready;  // not a player
    if (!ready) {
        NoteCheck(npc, mobile);
        return false;
    }
    return MayFire(npc, target, mobile);
}

void __fastcall HookDoneAttacking(void* conductor, std::uint8_t* attacker, std::uint8_t* target, int attackType) {
    g_origDoneAttacking(conductor, attacker, target, attackType);
    if (!attacker) return;
    const std::uint64_t handle = *reinterpret_cast<const std::uint64_t*>(attacker + kInstanceHandle);
    std::lock_guard<std::mutex> lock(g_mutex);
    for (Shooter& s : g_shooters)
        if (s.handle == handle && !s.holding) {
            if (g_log.load() && g_checkLogBudget.fetch_sub(1) > 0)
                Log::Detailf("EnemyAI: enemy %u reports an attack done (type %d) without a fire token", static_cast<unsigned>(handle), attackType);
            break;
        }
    for (Shooter& s : g_shooters)
        if (s.handle == handle && s.holding) {
            // Process_Start can report "done" for the action's previous state right after the yes: not the end of this attack.
            const std::uint32_t now = GameTimeMs();
            if (now - s.grantedMs >= kSameAttackMs) EndToken(s, now, "attack done");
            break;
        }
}
// ---------- melee tokens ----------
struct MeleeScan {
    std::uint8_t* cleared[8];   // tunes that lost the bit in this scan
    std::uint8_t* restored[8];  // tunes that got it back
    std::uint32_t flags[8];
    int clearedCount, restoredCount, enemies, asking;
    int attackersBefore, attackersNow;  // maxNumAttackers, -1 = no settings
    float meleeGapBefore, meleeGapNow;  // the melee attacks' minTimeBetween when it was changed (else -1)
};

// Every enemy in the level: its type's melee tune asks the BattleConductor (or, with the option off, is as the game
// made it). No C++ objects in here: the reads and writes are guarded.
void ScanMeleeTunes(bool on, int maxMelee, MeleeScan& r) {
    r.clearedCount = r.restoredCount = r.enemies = r.asking = 0;
    r.attackersBefore = r.attackersNow = -1;
    r.meleeGapBefore = r.meleeGapNow = -1.0f;
    __try {
        const std::uint8_t* npc = *At<const std::uint8_t* const*>(kRvaNpcList);
        for (int n = 0; npc && n < kMaxGuardNpcs; ++n, npc = *reinterpret_cast<const std::uint8_t* const*>(npc + kNpcNext)) {
            std::uint8_t* tune = *reinterpret_cast<std::uint8_t* const*>(npc + kNpcTune);
            if (!tune) continue;
            ++r.enemies;
            auto* flags = reinterpret_cast<std::uint32_t*>(tune + kTuneMeleeFlags);
            int known = -1;
            for (int i = 0; i < g_meleeTuneCount; ++i)
                if (g_meleeTunes[i] == tune) known = i;
            if (on && (*flags & kMeleeNoPermission)) {
                if (known < 0 && g_meleeTuneCount >= kMaxMeleeTunes) continue;
                if (r.clearedCount < 8) {
                    r.cleared[r.clearedCount] = tune;
                    r.flags[r.clearedCount] = *flags;
                }
                ++r.clearedCount;
                *flags &= ~kMeleeNoPermission;
                if (known < 0) g_meleeTunes[g_meleeTuneCount++] = tune;
            } else if (!on && known >= 0 && !(*flags & kMeleeNoPermission)) {
                *flags |= kMeleeNoPermission;
                if (r.restoredCount < 8) r.restored[r.restoredCount] = tune;
                ++r.restoredCount;
            }
            if (!(*flags & kMeleeNoPermission)) ++r.asking;
        }
        std::uint8_t* settings = *reinterpret_cast<std::uint8_t* const*>(At<const std::uint8_t*>(kRvaConductor) + kConductorSettings);
        if (settings && (!g_settingsGuardAvailable || g_settingsSafe)) {
            auto* attackers = reinterpret_cast<std::int32_t*>(settings);
            if (settings != g_settingsSeen) {  // new settings (a level's own, or the first read): this is the game's value
                g_settingsSeen = settings;
                g_settingsOriginal = *attackers;
            }
            // the melee attack type's entry of the first target settings (attack type 0): its minTimeBetween
            if (settings != g_settingsSeenMelee) {
                g_settingsSeenMelee = settings;
                g_meleeMinTime = nullptr;
                const std::uint8_t* target = *reinterpret_cast<const std::uint8_t* const*>(settings + 0x68);
                const std::uint32_t count = target ? *reinterpret_cast<const std::uint32_t*>(target) : 0;
                std::uint8_t* list = target ? *reinterpret_cast<std::uint8_t* const*>(target + 8) : nullptr;
                for (std::uint32_t i = 0; list && i < count && i < 32; ++i) {
                    std::uint8_t* entry = list + i * 0x18;
                    if (*reinterpret_cast<const std::uint16_t*>(entry) != 0) continue;
                    g_meleeMinTime = reinterpret_cast<float*>(entry + 4);
                    g_meleeMinTimeOriginal = *g_meleeMinTime;
                    break;
                }
            }
            if (g_meleeMinTime) {
                const float wantGap = on && g_meleeCalm.load() ? static_cast<float>(g_meleeCalmMeleeGap.load()) : g_meleeMinTimeOriginal;
                if (*g_meleeMinTime != wantGap && wantGap >= 0.0f && wantGap <= 30.0f) {
                    r.meleeGapBefore = *g_meleeMinTime;
                    *g_meleeMinTime = wantGap;
                    r.meleeGapNow = wantGap;
                }
            }
            r.attackersBefore = *attackers;
            const int want = on ? maxMelee : g_settingsOriginal;
            if (want >= 1 && want <= 8 && *attackers != want) *attackers = want;
            r.attackersNow = *attackers;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

bool ReadConductor(const std::uint8_t*& settings, std::uint8_t& flags) {
    __try {
        const std::uint8_t* conductor = At<const std::uint8_t*>(kRvaConductor);
        settings = *reinterpret_cast<const std::uint8_t* const*>(conductor + kConductorSettings);
        flags = conductor[kConductorSettingsFlags];
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool ResetConductorSettings() {
    __try {
        g_setDtpSettings(At<void*>(kRvaConductor), nullptr);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void SettingsGuardTick() {
    const std::uint8_t* settings = nullptr;
    std::uint8_t flags = 0;
    if (!ReadConductor(settings, flags)) return;
    if (!settings) {
        g_guardSettings = nullptr;
        g_guardSettingsDtp = 0;
        g_settingsSafe = false;
        return;
    }
    if (settings != g_guardSettings) {  // new settings: find their DTP (a scan of the DTP table, rare)
        g_guardSettings = settings;
        g_guardSettingsDtp = DtpEdit::FindContaining(settings, &g_guardSettingsData);
        g_settingsSafe = g_guardSettingsDtp != 0;
        if (g_settingsLogBudget.fetch_sub(1) > 0) {
            if (g_guardSettingsDtp)
                Log::Detailf("EnemyAI: battle settings %p (%s) are in DTP %u", settings,
                            (flags & kConductorCustomSettings) ? "an action graph's" : "the default", g_guardSettingsDtp);
            else
                Log::Detailf("EnemyAI: battle settings %p (%s) are in no loaded DTP that could be found: left as they are, "
                            "the framework does not write to them", settings, (flags & kConductorCustomSettings) ? "an action graph's" : "the default");
        }
        return;
    }
    if (!g_guardSettingsDtp || DtpEdit::StillResident(g_guardSettingsDtp, g_guardSettingsData)) return;
    // the DTP is gone (or loaded anew elsewhere): the pointer is stale
    g_settingsSafe = false;
    g_settingsSeen = g_settingsSeenMelee = nullptr;
    g_meleeMinTime = nullptr;
    const bool reset = ResetConductorSettings();
    g_settingsReset.fetch_add(1);
    const std::uint8_t* now = nullptr;
    std::uint8_t nowFlags = 0;
    ReadConductor(now, nowFlags);
    if (g_settingsLogBudget.fetch_sub(1) > 0)
        Log::Detailf("EnemyAI: battle settings guard: DTP %u (the %s battle settings at %p) was unloaded while the enemies still used "
                    "it; %s (now %p)", g_guardSettingsDtp, (flags & kConductorCustomSettings) ? "action graph's" : "default", settings,
                    reset ? "back to the default settings" : "the reset failed", now);
    g_guardSettings = nullptr;  // found again next tick
    g_guardSettingsDtp = 0;
}

void MeleeTick() {
    static ULONGLONG s_last = 0;
    static bool s_wasOn = false, s_stateLogged = false;
    const ULONGLONG tick = GetTickCount64();
    if (tick - s_last < 500) return;
    s_last = tick;
    // always, or (the default) while Lara throws a hand-to-hand combo: the enemies around her take turns then
    const bool brawl = g_meleeInBrawl.load() && (Brawl::ComboRunning() || Brawl::InMelee());
    const bool on = g_meleeTokens.load() || brawl;
    if (on != s_wasOn && g_meleeLogBudget.fetch_sub(1) > 0)
        Log::Detailf("EnemyAI: melee tokens %s%s", on ? "on" : "off", brawl ? " (a hand-to-hand combo runs)" : "");
    if (!on && !s_wasOn && g_meleeTuneCount == 0) return;  // never on: nothing of ours in the data
    MeleeScan r;
    ScanMeleeTunes(on, g_maxMelee.load(), r);
    s_wasOn = on;
    g_shownMeleeTunes.store(r.asking);
    for (int i = 0; i < r.clearedCount && i < 8; ++i)
        if (g_meleeLogBudget.fetch_sub(1) > 0)
            Log::Detailf("EnemyAI: melee tokens: an enemy type's melee needed no permission (tune %p, flags %08X): it asks now", r.cleared[i],
                        r.flags[i]);
    for (int i = 0; i < r.restoredCount && i < 8; ++i)
        if (g_meleeLogBudget.fetch_sub(1) > 0)
            Log::Detailf("EnemyAI: melee tokens off: tune %p needs no permission again, as the game made it", r.restored[i]);
    if (r.attackersBefore >= 0 && r.attackersBefore != r.attackersNow && g_meleeLogBudget.fetch_sub(1) > 0)
        Log::Detailf("EnemyAI: melee tokens: enemies in melee at once %d -> %d (the game's own value here: %d)", r.attackersBefore,
                    r.attackersNow, g_settingsOriginal);
    if (r.meleeGapNow >= 0.0f && g_meleeLogBudget.fetch_sub(1) > 0)
        Log::Detailf("EnemyAI: the enemies' melee attacks on Lara: time between two %.1f s -> %.1f s", r.meleeGapBefore, r.meleeGapNow);
    if (!s_stateLogged && r.enemies > 0) {
        s_stateLogged = true;
        Log::Detailf("EnemyAI: melee tokens %s: %d enemies in the level, %d of them ask before a melee attack; enemies in melee at once %d",
                    on ? "on" : "off", r.enemies, r.asking, r.attackersNow);
    }
}

// ---------- state control guard ----------
struct GuardFinding {
    unsigned enemy, agent, slot;
    int entry;
    std::uint64_t value;
    bool repaired;
};

bool FunctionOrNothing(std::uint64_t value) {
    return value == 0 || (value >= g_base && value < g_imageEnd) || (value >= g_ownBase && value < g_ownEnd);
}

std::uintptr_t ImageEnd(std::uintptr_t base) {
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
    return base + nt->OptionalHeader.SizeOfImage;
}

// Reads the function slots of a few enemy agents (the next ones each tick, all of them within a few ticks). No C++
// objects in here: the reads are guarded.
int ScanEnemyAgents(GuardFinding* findings, int capacity, int& agentsSeen, unsigned* oddBySlot) {
    constexpr int kAgentsPerTick = 8;
    static int s_cursor = 0;
    int found = 0, seen = 0;
    __try {
        const std::uint8_t* npc = *At<const std::uint8_t* const*>(kRvaNpcList);
        for (int n = 0; npc && n < kMaxGuardNpcs; ++n, npc = *reinterpret_cast<const std::uint8_t* const*>(npc + kNpcNext)) {
            const std::uint8_t* instance = *reinterpret_cast<const std::uint8_t* const*>(npc + kNpcInstance);
            if (!instance) continue;
            const std::uint64_t handle = *reinterpret_cast<const std::uint64_t*>(instance + kInstanceHandle);
            if (!handle) continue;
            std::uint8_t* component = g_tryToGetStateControl(handle);
            if (!component) continue;
            const std::uint8_t* info = *reinterpret_cast<const std::uint8_t* const*>(component + kComponentAgentInfo);
            if (!info) continue;
            const std::uint32_t count = (std::min)(*reinterpret_cast<const std::uint32_t*>(info + 8), kMaxGuardAgents);
            for (std::uint32_t a = 0; a < count; ++a, ++seen) {
                if (seen < s_cursor || seen >= s_cursor + kAgentsPerTick) continue;
                std::uint8_t* agent = g_getAgent(component, a);
                if (!agent) continue;
                for (int e = 0; e < kEnumEntries; ++e) {
                    std::uint8_t* entry = agent + kAgentEnumData + static_cast<std::size_t>(e) * kEnumEntrySize;
                    for (std::size_t slot = kEnumFirstFunction; slot <= kEnumLastFunction; slot += 8) {
                        auto* p = reinterpret_cast<std::uint64_t*>(entry + slot);
                        const std::uint64_t value = *p;
                        if (FunctionOrNothing(value)) continue;
                        bool repaired = false;
                        if ((value >> 32) == 0) {
                            const std::uint64_t whole = (static_cast<std::uint64_t>(g_base) & 0xFFFFFFFF00000000ull) | value;
                            if (whole >= g_base && whole < g_imageEnd) {
                                *p = whole;
                                repaired = true;
                            }
                        }
                        if (!repaired) {  // not an address that lost its upper half: counted, left alone
                            ++oddBySlot[(slot - kEnumFirstFunction) / 8];
                            continue;
                        }
                        if (found < capacity)
                            findings[found] = GuardFinding{static_cast<unsigned>(handle), a, static_cast<unsigned>(slot), e, value, repaired};
                        ++found;
                    }
                }
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    s_cursor = seen > s_cursor + kAgentsPerTick ? s_cursor + kAgentsPerTick : 0;
    agentsSeen = seen;
    return found;
}

void GuardTick() {
    static const char* const kSlotNames[] = {"condition", "property", "entry", "re-entry", "pre-anim entry", "pre-anim re-entry", "exit", "process"};
    GuardFinding findings[16];
    unsigned odd[8] = {};
    int agents = 0;
    const int found = ScanEnemyAgents(findings, 16, agents, odd);
    static bool s_firstScanLogged = false;
    if (!s_firstScanLogged && agents > 0) {
        s_firstScanLogged = true;
        Log::Detailf("EnemyAI: state control guard: first scan, %d enemy agents; values that are no functions, per slot (condition .. process): "
                    "%u %u %u %u %u %u %u %u",
                    agents, odd[0], odd[1], odd[2], odd[3], odd[4], odd[5], odd[6], odd[7]);
    }
    if (found <= 0) return;
    g_guardFound.fetch_add(static_cast<unsigned>(found));
    g_guardRepaired.fetch_add(static_cast<unsigned>(found));
    for (int i = 0; i < found && i < 16; ++i) {
        const GuardFinding& f = findings[i];
        if (g_guardLogBudget.fetch_sub(1) <= 0) break;
        Log::Writef("EnemyAI: state control guard: enemy %u agent %u, enum %d, %s function = %016llX: the upper half of the address was lost, "
                    "put back",
                    f.enemy, f.agent, f.entry, kSlotNames[(f.slot - kEnumFirstFunction) / 8], static_cast<unsigned long long>(f.value));
    }
}
}  // namespace

bool Initialize() {
    if (!GameBuild::IsSupportedRetail()) return false;
    g_base = GameBuild::Base();
    LoadSettings();
    bool ok = Verify(kRvaAreWeReadyToFire, kAreWeReadyToFireSig, sizeof(kAreWeReadyToFireSig), "Action_Attack_Ranged::AreWeReadyToFire") &&
              Verify(kRvaGameTimeUse, kGameTimeUseSig, sizeof(kGameTimeUseSig), "game time load") &&
              Verify(kRvaGameTimeUse + 6, kGameTimeUseTailSig, sizeof(kGameTimeUseTailSig), "game time compare") &&
              Verify(kRvaDoneAttacking, kDoneAttackingSig, sizeof(kDoneAttackingSig), "BattleConductor::DoneAttacking") &&
              Verify(kRvaNpcInstanceUse, kNpcInstanceUseSig, sizeof(kNpcInstanceUseSig), "NpcComponent instance offset") &&
              Verify(kRvaNpcOnCameraUse, kNpcOnCameraUseSig, sizeof(kNpcOnCameraUseSig), "NpcComponent on camera time offset");
    if (ok && RelTarget(kRvaGameTimeUse, 2, 6) != kRvaGameTime) {
        Log::Write("EnemyAI: game time reference mismatch; enemy fire tokens unavailable");
        ok = false;
    }
    if (ok) {
        void* ready = At<void*>(kRvaAreWeReadyToFire);
        void* done = At<void*>(kRvaDoneAttacking);
        if (MH_CreateHook(ready, reinterpret_cast<void*>(&HookAreWeReadyToFire), reinterpret_cast<void**>(&g_origAreWeReadyToFire)) != MH_OK ||
            MH_CreateHook(done, reinterpret_cast<void*>(&HookDoneAttacking), reinterpret_cast<void**>(&g_origDoneAttacking)) != MH_OK ||
            MH_EnableHook(ready) != MH_OK || MH_EnableHook(done) != MH_OK) {
            Log::Write("EnemyAI: failed to install the ranged attack hooks; enemy fire tokens unavailable");
            MH_DisableHook(ready);
            MH_DisableHook(done);
            ok = false;
        }
    }
    g_imageEnd = ImageEnd(g_base);
    HMODULE own = nullptr;
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCSTR>(&Initialize), &own) &&
        own) {
        g_ownBase = reinterpret_cast<std::uintptr_t>(own);
        g_ownEnd = ImageEnd(g_ownBase);
    }
    g_guardAvailable = GameBuild::VerifyBytes(kRvaTryToGetStateControl, kTryToGetStateControlSig, sizeof(kTryToGetStateControlSig)) &&
                       GameBuild::VerifyBytes(kRvaGetAgent, kGetAgentSig, sizeof(kGetAgentSig)) &&
                       GameBuild::VerifyBytes(kRvaNpcListLoad, kNpcListLoadSig, sizeof(kNpcListLoadSig)) &&
                       GameBuild::VerifyBytes(kRvaNpcListNextUse, kNpcListNextUseSig, sizeof(kNpcListNextUseSig)) &&
                       GameBuild::VerifyBytes(kRvaNpcInstanceUse, kNpcInstanceUseSig, sizeof(kNpcInstanceUseSig)) &&
                       RelTarget(kRvaNpcListLoad, 3, 7) == kRvaNpcList && g_ownBase != 0;
    if (g_guardAvailable) {
        g_tryToGetStateControl = At<TryToGetStateControlFn>(kRvaTryToGetStateControl);
        g_getAgent = At<GetAgentFn>(kRvaGetAgent);
        Log::Detailf("EnemyAI: state control guard %s (the function tables of the enemies' agents, a few agents each tick)",
                    g_guard.load() ? "on" : "off");
    } else {
        Log::Write("EnemyAI: state control guard unavailable (the game's code did not match)");
    }
    g_meleeAvailable = GameBuild::VerifyBytes(kRvaNpcTuneUse, kNpcTuneUseSig, sizeof(kNpcTuneUseSig)) &&
                       GameBuild::VerifyBytes(kRvaMeleeTuneUse, kMeleeTuneUseSig, sizeof(kMeleeTuneUseSig)) &&
                       GameBuild::VerifyBytes(kRvaMeleePermissionUse, kMeleePermissionUseSig, sizeof(kMeleePermissionUseSig)) &&
                       GameBuild::VerifyBytes(kRvaConductorUse, kConductorUseSig, sizeof(kConductorUseSig)) &&
                       GameBuild::VerifyBytes(kRvaNpcListLoad, kNpcListLoadSig, sizeof(kNpcListLoadSig)) &&
                       GameBuild::VerifyBytes(kRvaNpcListNextUse, kNpcListNextUseSig, sizeof(kNpcListNextUseSig)) &&
                       RelTarget(kRvaConductorUse, 3, 7) == kRvaConductor && RelTarget(kRvaNpcListLoad, 3, 7) == kRvaNpcList;
    g_settingsGuardAvailable = g_meleeAvailable && DtpEdit::Initialize() &&
                               GameBuild::VerifyBytes(kRvaSetDtpSettings, kSetDtpSettingsSig, sizeof(kSetDtpSettingsSig));
    if (g_settingsGuardAvailable) {
        g_setDtpSettings = At<SetDtpSettingsFn>(kRvaSetDtpSettings);
        Log::Detail("EnemyAI: battle settings guard on (the enemies' battle settings go back to the default when their DTP unloads)");
    } else {
        Log::Write("EnemyAI: battle settings guard unavailable (the game's code did not match, or game data edits are off)");
    }
    if (g_meleeAvailable)
        Log::Detailf("EnemyAI: melee tokens %s (enemies in melee at once: %d)", g_meleeTokens.load() ? "on" : "off", g_maxMelee.load());
    else
        Log::Write("EnemyAI: melee tokens unavailable (the game's code did not match)");
    g_available = ok;
    if (ok)
        Log::Detailf("EnemyAI: ranged attack hooks installed; fire tokens %s (at most %d shooting at once, %d when %d or more want to; "
                    "gap %d ms, per enemy cooldown %d ms, on camera first %s; near = within %d cm, near cooldown %d ms, far turn every %d ms)",
                    g_fireTokens.load() ? "on" : "off (counting only)", g_maxShooters.load(), g_maxShootersLargeGroup.load(),
                    g_largeGroup.load(), g_gapMs.load(), g_npcCooldownMs.load(), g_preferOnCamera.load() ? "on" : "off",
                    g_closeRange.load(), g_closeCooldownMs.load(), g_farTurnMs.load());
    return ok;
}

void TickGameThread() {
    if (g_settingsGuardAvailable) SettingsGuardTick();
    if (g_guardAvailable && g_guard.load()) GuardTick();
    if (g_meleeAvailable) MeleeTick();
    if (!g_available) return;
    const ULONGLONG tick = GetTickCount64();
    if (tick - g_periodStart < kSummaryEveryMs) return;
    Period p;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        p = g_period;
        g_period = Period{};
        // Nobody asks while there is no fight: let the menu's numbers fall back.
        unsigned holding = 0, wanting = 0;
        Refresh(GameTimeMs(), holding, wanting);
        g_shownHolding.store(static_cast<int>(holding));
        g_shownWanting.store(static_cast<int>(wanting));
    }
    g_periodStart = tick;
    unsigned refused = 0;
    for (unsigned n : p.refusals) refused += n;
    if (!p.grants && !refused) return;
    if (!g_log.load() || g_summaryLogBudget.fetch_sub(1) <= 0) return;
    Log::Detailf("EnemyAI: last 5 s (fire tokens %s): up to %u enemies wanted to shoot Lara, %u attacks started, most shooting at once %u; "
                "refused checks: %s %u, %s %u, %s %u, %s %u, %s %u",
                g_fireTokens.load() ? "on" : "off", p.mostWanting, p.grants, p.mostAtOnce, kRefusalNames[0], p.refusals[0],
                kRefusalNames[1], p.refusals[1], kRefusalNames[2], p.refusals[2], kRefusalNames[3], p.refusals[3], kRefusalNames[4],
                p.refusals[4]);
    if (p.refusals[kLaraInMelee] && g_log.load())
        Log::Detailf("EnemyAI: last 5 s: %u checks refused because Lara fights hand to hand (melee calm)", p.refusals[kLaraInMelee]);
}

void DrawPlayerSection() {
    // (v0.9.519: its title is the Enhancements tab's collapsing header)
    if (!g_available) ImGui::BeginDisabled();
    bool tokens = g_fireTokens.load();
    if (ImGui::Checkbox("Fire tokens: the enemies take turns shooting Lara", &tokens)) {
        g_fireTokens.store(tokens);
        SaveSettings();
        Log::Detailf("EnemyAI: fire tokens turned %s in the menu", tokens ? "on" : "off (counting only)");
    }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
        ImGui::SetTooltip(
            "In the game every enemy that sees Lara shoots at the same time: its attack limit only covers melee.\n"
            "With this on, an enemy that is ready to fire needs a token; the others keep aiming and wait their turn.\n"
            "The enemy near Lara goes first, the far ones wait while it fights. Melee, grenades and the enemies'\n"
            "movement are unchanged. Off: nothing is refused, the log still counts who shoots.");
    }
    if (tokens) {
        ImGui::Indent();
        int value = g_maxShooters.load();
        ImGui::SetNextItemWidth(260.0f);
        if (ImGui::SliderInt("Enemies shooting at once", &value, 1, 6)) g_maxShooters.store(value);
        if (ImGui::IsItemDeactivatedAfterEdit()) SaveSettings();
        value = g_maxShootersLargeGroup.load();
        ImGui::SetNextItemWidth(260.0f);
        if (ImGui::SliderInt("... in a large group", &value, 1, 6)) g_maxShootersLargeGroup.store(value);
        if (ImGui::IsItemDeactivatedAfterEdit()) SaveSettings();
        value = g_largeGroup.load();
        ImGui::SetNextItemWidth(260.0f);
        if (ImGui::SliderInt("Large group: enemies wanting to shoot", &value, 2, 20)) g_largeGroup.store(value);
        if (ImGui::IsItemDeactivatedAfterEdit()) SaveSettings();
        value = g_gapMs.load();
        ImGui::SetNextItemWidth(260.0f);
        if (ImGui::SliderInt("Pause between two enemies' attacks (ms)", &value, 0, 5000)) g_gapMs.store(value);
        if (ImGui::IsItemDeactivatedAfterEdit()) SaveSettings();
        value = g_npcCooldownMs.load();
        ImGui::SetNextItemWidth(260.0f);
        if (ImGui::SliderInt("Same enemy shoots again after (ms)", &value, 0, 15000)) g_npcCooldownMs.store(value);
        if (ImGui::IsItemDeactivatedAfterEdit()) SaveSettings();
        value = g_closeRange.load();
        ImGui::SetNextItemWidth(260.0f);
        if (ImGui::SliderInt("Near enemy: within (cm)", &value, 150, 3000)) g_closeRange.store(value);
        if (ImGui::IsItemDeactivatedAfterEdit()) SaveSettings();
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("An enemy this near Lara is the one that fights her: it does not wait for the far ones' token, and while\n"
                              "it wants to shoot the far enemies wait.");
        value = g_closeCooldownMs.load();
        ImGui::SetNextItemWidth(260.0f);
        if (ImGui::SliderInt("Near enemy shoots again after (ms)", &value, 0, 10000)) g_closeCooldownMs.store(value);
        if (ImGui::IsItemDeactivatedAfterEdit()) SaveSettings();
        value = g_farTurnMs.load();
        ImGui::SetNextItemWidth(260.0f);
        if (ImGui::SliderInt("Far enemies' turn while a near one fights, every (ms)", &value, 0, 30000)) g_farTurnMs.store(value);
        if (ImGui::IsItemDeactivatedAfterEdit()) SaveSettings();
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("0: the far enemies never shoot while a near enemy wants to.");
        bool onCamera = g_preferOnCamera.load();
        if (ImGui::Checkbox("Enemies on camera shoot first", &onCamera)) {
            g_preferOnCamera.store(onCamera);
            SaveSettings();
        }
        ImGui::Unindent();
    }
    bool log = g_log.load();
    if (ImGui::Checkbox("Log the enemies' shooting (ROTTRFramework.log)", &log)) {
        g_log.store(log);
        SaveSettings();
    }
    if (!g_available) ImGui::EndDisabled();
    if (!g_meleeAvailable) ImGui::BeginDisabled();
    bool melee = g_meleeTokens.load();
    if (ImGui::Checkbox("Melee tokens: the enemies take turns hitting Lara", &melee)) {
        g_meleeTokens.store(melee);
        SaveSettings();
        Log::Detailf("EnemyAI: melee tokens turned %s in the menu", melee ? "on" : "off");
    }
    bool calmOn = g_meleeCalm.load();
    if (ImGui::Checkbox("The enemies hold back while Lara fights hand to hand", &calmOn)) {
        g_meleeCalm.store(calmOn);
        SaveSettings();
    }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("While a punch / kick combo or the axe's attack runs: no shots from the far enemies and none from the\n"
                          "enemy she is hitting, the other near ones shoot one at a time with a pause between them, and the\n"
                          "enemies' own melee attacks wait a few seconds after each other.");
    if (calmOn) {
        ImGui::Indent();
        int gap = g_meleeCalmGapMs.load();
        ImGui::SetNextItemWidth(260.0f);
        if (ImGui::SliderInt("Pause between two near shooters (ms)", &gap, 0, 10000)) g_meleeCalmGapMs.store(gap);
        if (ImGui::IsItemDeactivatedAfterEdit()) SaveSettings();
        int hold = g_meleeCalmHoldMs.load();
        ImGui::SetNextItemWidth(260.0f);
        if (ImGui::SliderInt("A near shooter's burst, at most (ms)", &hold, 300, 6000)) g_meleeCalmHoldMs.store(hold);
        if (ImGui::IsItemDeactivatedAfterEdit()) SaveSettings();
        int meleeGap = g_meleeCalmMeleeGap.load();
        ImGui::SetNextItemWidth(260.0f);
        if (ImGui::SliderInt("Time between two melee attacks on her (s)", &meleeGap, 0, 20)) g_meleeCalmMeleeGap.store(meleeGap);
        if (ImGui::IsItemDeactivatedAfterEdit()) SaveSettings();
        ImGui::Text("Shots refused for it so far: %u", g_meleeCalmRefusals.load());
        ImGui::Unindent();
    }
    bool inBrawl = g_meleeInBrawl.load();
    if (ImGui::Checkbox("Melee tokens while Lara throws a hand-to-hand combo", &inBrawl)) {
        g_meleeInBrawl.store(inBrawl);
        SaveSettings();
    }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("The game lets one enemy at a time attack in melee, but the soldiers with guns are set to hit without\n"
                          "asking: next to Lara they all strike together. On: they ask like the melee enemies do, and the others\n"
                          "wait for their turn.");
    if (melee || inBrawl) {
        ImGui::Indent();
        int attackers = g_maxMelee.load();
        ImGui::SetNextItemWidth(260.0f);
        if (ImGui::SliderInt("Enemies in melee at once", &attackers, 1, 4)) g_maxMelee.store(attackers);
        if (ImGui::IsItemDeactivatedAfterEdit()) SaveSettings();
        ImGui::Unindent();
    }
    ImGui::Text("Melee: %d enemies here ask before they hit.", g_shownMeleeTunes.load());
    if (!g_meleeAvailable) ImGui::EndDisabled();
    if (!g_guardAvailable) ImGui::BeginDisabled();
    bool guard = g_guard.load();
    if (ImGui::Checkbox("Guard the enemies' state control tables", &guard)) {
        g_guard.store(guard);
        SaveSettings();
    }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("The v0.9.247 test crashed when an enemy entered its finisher state: a function address in its state\n"
                          "control table had lost its upper half. The guard checks those tables, writes what it finds to the\n"
                          "log and puts a damaged address back.");
    ImGui::Text("Guard: %u damaged addresses put back.", g_guardRepaired.load());
    if (!g_guardAvailable) ImGui::EndDisabled();
    if (!g_available) ImGui::BeginDisabled();
    ImGui::Text("Now: %d shooting, %d wanting to shoot. Attacks started %u, refused checks %u.", g_shownHolding.load(),
                g_shownWanting.load(), g_totalGrants.load(), g_totalRefusals.load());
    if (!g_available) {
        ImGui::EndDisabled();
        ImGui::TextUnformatted("Unavailable: the game's code did not match (see ROTTRFramework.log).");
    }
}
}  // namespace EnemyAI
