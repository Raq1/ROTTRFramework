#include "weapon_mods.h"

#include "dtp_edit.h"
#include "game_build.h"
#include "log.h"

#include <Windows.h>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <deque>
#include <memory>
#include <string>
#include <vector>

namespace WeaponMods {
namespace {
namespace fs = std::filesystem;
using Json = nlohmann::json;

constexpr std::uintptr_t kRvaGlobalUIInfoPtr = 0x02E1E720ull;     // dtp::GlobalUIInfo*
constexpr std::uintptr_t kRvaGlobalCraftingPtr = 0x02D57430ull;   // dtp::GlobalCrafting*
// UiMenuStateCampsiteWeaponUpgrades' read of the upgrade table: mov rcx, [rip + GlobalWeaponUpgrades*]; cmp [rcx], 0
constexpr std::uintptr_t kRvaWeaponUpgradesRead = 0x008E6F40ull;
constexpr std::uint8_t kWeaponUpgradesReadSig[] = {0x48, 0x8B, 0x0D, 0x21, 0x19, 0x47, 0x02, 0x83, 0x39, 0x00, 0x76, 0x37};
// PlayerTraits::GetTrait: mov rdx, [rip + GlobalAchievements*]; ...; cmp ecx, [rdx + 0x20]
constexpr std::uintptr_t kRvaGetTrait = 0x0044D5A0ull;
constexpr std::uint8_t kGetTraitSig[] = {0x48, 0x8B, 0x15, 0xF9, 0x9E, 0x90, 0x02, 0x48, 0x85, 0xD2, 0x74, 0x18,
                                         0x85, 0xC9, 0x78, 0x14, 0x3B, 0x4A, 0x20, 0x73, 0x0F, 0x48, 0x63, 0xC1};

constexpr std::size_t kUiWeaponCount = 0x1060, kUiWeaponList = 0x1068;
constexpr std::size_t kInfoStride = 0x50, kInfoObject = 0x04, kInfoUiObject = 0x06, kInfoName = 0x08, kInfoDescription = 0x0A, kInfoIcon = 0x10,
                      kInfoTrait = 0x20, kInfoTraitEndurance = 0x24, kInfoBars = 0x28;
constexpr std::size_t kUpgradeCount = 0x00, kUpgradeRecords = 0x08, kRecordStride = 0x20, kRecordObject = 0x00;
constexpr std::size_t kTraitCount = 0x20, kTraitList = 0x28, kTraitStride = 0x1C0, kTraitName = 0x00, kTraitDescription = 0x02,
                      kTraitRecipe = 0x14, kTraitIcon = 0x100;
constexpr std::size_t kRecipeCount = 0x00, kRecipeList = 0x08, kRecipeStride = 0xC0, kRecipeName = 0x00, kRecipeDescription = 0x02;
constexpr int kOwner = DtpEdit::kSuiteOwner + 2;   // DtpEdit's owner of the mods' values (VoiceEdit is + 1)

struct WeaponConfig {
    std::string mod, key;
    std::string object, base, unlock;
    Text name, description;
    std::string icon;
    bool hasBars = false;
    float bars[6]{};
    fs::path folder;
    // (v0.9.569) its own campsite model: <object>_ui built from uiBase (base's campsite model) and uiFolder's files
    std::string uiObject, uiBase;
    fs::path uiFolder;
    std::uint16_t zone = 0, baseZone = 0, unlockZone = 0, uiZone = 0, uiBaseZone = 0;
    bool resolved = false, reported = false;
    bool IsNew() const { return !base.empty(); }
};
struct UpgradeConfig {
    std::string mod;
    std::uint32_t upgrade = 0;
    std::uint16_t trait = 0xFFFF, previewTrait = 0xFFFF;
    Text name, description;
    std::string icon;
};
struct ValueConfig {
    std::string mod;
    std::uint32_t dtp = 0, offset = 0, size = 0;
    std::string type, value, original, label;
};

std::deque<WeaponConfig> g_weapons;     // (deques: the Text pointers handed out stay valid)
std::deque<UpgradeConfig> g_upgrades;
std::vector<ValueConfig> g_values;
std::deque<std::string> g_icons;        // icon labels the game's data is pointed at
bool g_valuesSet = false;
bool g_dtpEditMissing = false;
std::string g_status = "no weapons in the mods";

// the weapon list and the upgrade table: ours (copies) and the game's counts under them
std::vector<std::unique_ptr<std::uint8_t[]>> g_buffers;   // kept: the game may hold pointers into them
std::uint8_t* g_list = nullptr;
std::uint32_t g_listCount = 0, g_listGameCount = 0;
std::uint8_t* g_records = nullptr;
std::uint32_t g_recordCount = 0, g_recordGameCount = 0;
std::uintptr_t g_upgradesGlobal = 0, g_traitsGlobal = 0;
bool g_globalsChecked = false;
ULONGLONG g_lastTexts = 0;

std::string Lower(std::string text) {
    for (char& c : text) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return text;
}
std::string JsonText(const Json& o, const char* key) {
    auto it = o.find(key);
    return it != o.end() && it->is_string() ? it->get<std::string>() : std::string();
}
std::uint32_t JsonNumber(const Json& o, const char* key, std::uint32_t fallback) {
    auto it = o.find(key);
    return it != o.end() && it->is_number_unsigned() ? it->get<std::uint32_t>() : fallback;
}
fs::path PathFromUtf8(const std::string& text) { return fs::path(std::u8string(text.begin(), text.end())); }

bool CopyGuarded(void* destination, const void* source, std::size_t size) noexcept {
    __try {
        std::memcpy(destination, source, size);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
template <typename T> bool ReadAt(std::uintptr_t address, T& out) { return address && CopyGuarded(&out, reinterpret_cast<const void*>(address), sizeof(T)); }
template <typename T> void Put(std::uint8_t* p, std::size_t offset, T value) { std::memcpy(p + offset, &value, sizeof(T)); }
template <typename T> T Take(const std::uint8_t* p, std::size_t offset) {
    T v{};
    std::memcpy(&v, p + offset, sizeof(T));
    return v;
}

const char* StoredIcon(const std::string& label) {
    for (const auto& s : g_icons)
        if (s == label) return s.c_str();
    g_icons.push_back(label);
    return g_icons.back().c_str();
}

bool FolderHasResources(const fs::path& folder) {
    std::error_code ec;
    if (folder.empty() || !fs::is_directory(folder, ec)) return false;
    for (auto it = fs::recursive_directory_iterator(folder, ec); it != fs::recursive_directory_iterator(); it.increment(ec)) {
        if (ec) break;
        if (!it->is_regular_file(ec)) continue;
        const std::string ext = Lower(it->path().extension().string());
        if (ext.rfind(".tr10", 0) == 0 || ext == ".dds") return true;
    }
    return false;
}

void CheckGlobals() {
    if (g_globalsChecked) return;
    g_globalsChecked = true;
    const std::uintptr_t base = GameBuild::Base();
    auto rip = [&](std::uintptr_t rva) {
        std::int32_t rel = 0;
        std::memcpy(&rel, reinterpret_cast<const void*>(base + rva + 3), 4);
        return base + rva + 7 + static_cast<std::intptr_t>(rel);
    };
    if (GameBuild::VerifyBytes(kRvaWeaponUpgradesRead, kWeaponUpgradesReadSig, sizeof(kWeaponUpgradesReadSig)))
        g_upgradesGlobal = rip(kRvaWeaponUpgradesRead);
    else
        Log::Write("WeaponMods: the weapon upgrade table's read does not match this game build: new weapons get no upgrades");
    if (GameBuild::VerifyBytes(kRvaGetTrait, kGetTraitSig, sizeof(kGetTraitSig)))
        g_traitsGlobal = rip(kRvaGetTrait);
    else
        Log::Write("WeaponMods: PlayerTraits::GetTrait does not match this game build: upgrades keep their texts and icons");
}

// The zones of the configs' objects (the new weapons' are registered by CustomOutfits before this runs)
void Resolve(const std::function<std::uint16_t(const std::string&)>& findZone) {
    for (auto& w : g_weapons) {
        if (w.resolved) continue;
        w.resolved = true;
        w.zone = findZone(w.object);
        if (w.IsNew()) {
            w.baseZone = findZone(w.base);
            w.unlockZone = w.unlock.empty() ? w.baseZone : findZone(w.unlock);
            if (!w.uiObject.empty()) {
                w.uiZone = findZone(w.uiObject);
                w.uiBaseZone = findZone(w.uiBase);
                if (!w.uiZone)
                    Log::Writef("WeaponMods: %s: its campsite model %s was not built (the campsite shows %s's)", w.key.c_str(),
                                w.uiObject.c_str(), w.base.c_str());
            }
        }
        if (!w.zone || (w.IsNew() && (!w.baseZone || !w.unlockZone)))
            Log::Writef("WeaponMods: %s: %s is not an object the game knows%s", w.key.c_str(),
                        !w.zone ? w.object.c_str() : !w.baseZone ? w.base.c_str() : w.unlock.c_str(),
                        !w.zone && w.IsNew() ? " (its object was not built: see the archive's lines above)" : "");
    }
}

void ApplyValues() {
    if (g_valuesSet || g_values.empty()) return;
    if (!DtpEdit::Initialize()) {
        if (!g_dtpEditMissing) Log::Writef("WeaponMods: the mods' %zu value(s) cannot be set: %s", g_values.size(), DtpEdit::Status());
        g_dtpEditMissing = true;
        return;
    }
    g_valuesSet = true;
    int applied = 0, pending = 0, refused = 0;
    for (const auto& v : g_values) {
        DtpEdit::Field field;
        std::string error;
        if (!DtpEdit::ParseField(v.dtp, v.offset, v.type, field, error)) {
            ++refused;
            Log::Writef("WeaponMods: %s: %s (DTP %u +0x%X): %s", v.mod.c_str(), v.label.c_str(), v.dtp, v.offset, error.c_str());
            continue;
        }
        std::vector<std::string> expect;
        if (!v.original.empty()) expect.push_back(v.original);
        switch (DtpEdit::SetOverride(kOwner, field, v.value, expect, v.size, error)) {
            case DtpEdit::SetResult::Applied: ++applied; break;
            case DtpEdit::SetResult::Pending: ++pending; break;
            case DtpEdit::SetResult::Refused:
                ++refused;
                Log::Writef("WeaponMods: %s: %s (DTP %u +0x%X) refused: %s", v.mod.c_str(), v.label.c_str(), v.dtp, v.offset, error.c_str());
                break;
        }
    }
    Log::Writef("WeaponMods: %zu value(s) of the game's weapon data set by mods: %d written, %d written when their data loads, %d refused",
                g_values.size(), applied, pending, refused);
}

void ApplyTexts(std::uint8_t* entry, const WeaponConfig& w) {
    if (w.name.id >= 0) Put<std::int16_t>(entry, kInfoName, static_cast<std::int16_t>(w.name.id));
    if (w.description.id >= 0) Put<std::int16_t>(entry, kInfoDescription, static_cast<std::int16_t>(w.description.id));
    if (!w.icon.empty()) Put<const char*>(entry, kInfoIcon, StoredIcon(w.icon));
    if (w.hasBars)
        for (int b = 0; b < 6; ++b) Put<float>(entry, kInfoBars + 4 * std::size_t(b), w.bars[b]);
}

// The campsite's weapon list: a copy with the mods' changes and new weapons
void ApplyWeaponList() {
    if (g_weapons.empty()) return;
    std::uint8_t* ui = nullptr;
    if (!ReadAt(GameBuild::Base() + kRvaGlobalUIInfoPtr, ui) || !ui) return;
    std::uint8_t* list = nullptr;
    std::uint32_t count = 0;
    if (!ReadAt(reinterpret_cast<std::uintptr_t>(ui + kUiWeaponList), list) || !ReadAt(reinterpret_cast<std::uintptr_t>(ui + kUiWeaponCount), count) ||
        !list || !count || count > 1024)
        return;
    if (list == g_list && count == g_listCount) return;   // ours, unchanged
    if (list == g_list) count = g_listGameCount;            // the count reset under our list: the game's part
    std::vector<const WeaponConfig*> added;
    for (const auto& w : g_weapons)
        if (w.IsNew() && w.zone && w.baseZone && w.unlockZone) added.push_back(&w);
    const std::uint32_t total = count + static_cast<std::uint32_t>(added.size());
    auto buffer = std::make_unique<std::uint8_t[]>(std::size_t(total) * kInfoStride);
    if (!CopyGuarded(buffer.get(), list, std::size_t(count) * kInfoStride)) return;
    auto entryOf = [&](std::uint16_t zone) -> std::uint8_t* {
        for (std::uint32_t i = 0; i < count; ++i)
            if (Take<std::uint16_t>(buffer.get() + std::size_t(i) * kInfoStride, kInfoObject) == zone) return buffer.get() + std::size_t(i) * kInfoStride;
        return nullptr;
    };
    int changed = 0;
    for (auto& w : g_weapons) {
        if (w.IsNew() || !w.zone) continue;
        bool found = false;
        for (std::uint32_t i = 0; i < count; ++i) {
            std::uint8_t* entry = buffer.get() + std::size_t(i) * kInfoStride;
            if (Take<std::uint16_t>(entry, kInfoObject) != w.zone) continue;
            ApplyTexts(entry, w);
            found = true;
        }
        changed += found ? 1 : 0;
        if (!found && !w.reported) Log::Writef("WeaponMods: %s: the campsite's weapon list has no %s", w.key.c_str(), w.object.c_str());
        w.reported = true;
    }
    std::uint32_t next = count;
    for (const WeaponConfig* w : added) {
        const std::uint8_t* base = entryOf(w->baseZone);
        const std::uint8_t* unlock = entryOf(w->unlockZone);
        if (!base || !unlock) {
            Log::WriteOncef("WeaponMods: %s: the campsite's weapon list has no %s", w->key.c_str(), (!base ? w->base : w->unlock).c_str());
            continue;
        }
        std::uint8_t* entry = buffer.get() + std::size_t(next) * kInfoStride;
        std::memcpy(entry, base, kInfoStride);
        Put<std::uint16_t>(entry, kInfoObject, w->zone);   // (m_uiObjectID: its own campsite model, else base's)
        if (w->uiZone) Put<std::uint16_t>(entry, kInfoUiObject, w->uiZone);
        Put<std::uint32_t>(entry, kInfoTrait, Take<std::uint32_t>(unlock, kInfoTrait));
        Put<std::uint32_t>(entry, kInfoTraitEndurance, Take<std::uint32_t>(unlock, kInfoTraitEndurance));
        ApplyTexts(entry, *w);
        ++next;
    }
    if (next == count && !changed) return;   // (nothing of ours could go in)
    std::uint8_t* ours = buffer.get();
    g_buffers.push_back(std::move(buffer));
    // the list first (the game's entries keep their places), then the count
    Put<std::uint8_t*>(ui, kUiWeaponList, ours);
    Put<std::uint32_t>(ui, kUiWeaponCount, next);
    g_list = ours;
    g_listCount = next;
    g_listGameCount = count;
    Log::Writef("WeaponMods: the campsite's weapon list: %d of the game's %u weapons changed, %u new weapon(s) after them",
                changed, count, next - count);
}

// The upgrade table: a record per new weapon (its base's upgrades)
void ApplyUpgradeTable() {
    if (!g_upgradesGlobal) return;
    std::vector<const WeaponConfig*> added;
    for (const auto& w : g_weapons)
        if (w.IsNew() && w.zone && w.baseZone) added.push_back(&w);
    if (added.empty()) return;
    std::uint8_t* table = nullptr;
    if (!ReadAt(g_upgradesGlobal, table) || !table) return;
    std::uint8_t* records = nullptr;
    std::uint32_t count = 0;
    if (!ReadAt(reinterpret_cast<std::uintptr_t>(table + kUpgradeRecords), records) ||
        !ReadAt(reinterpret_cast<std::uintptr_t>(table + kUpgradeCount), count) || !records || !count || count > 4096)
        return;
    if (records == g_records && count == g_recordCount) return;
    if (records == g_records) count = g_recordGameCount;
    const std::uint32_t total = count + static_cast<std::uint32_t>(added.size());
    auto buffer = std::make_unique<std::uint8_t[]>(std::size_t(total) * kRecordStride);
    if (!CopyGuarded(buffer.get(), records, std::size_t(count) * kRecordStride)) return;
    std::uint32_t next = count;
    for (const WeaponConfig* w : added) {
        const std::uint8_t* base = nullptr;
        for (std::uint32_t i = 0; i < count && !base; ++i)
            if (Take<std::uint16_t>(buffer.get() + std::size_t(i) * kRecordStride, kRecordObject) == w->baseZone)
                base = buffer.get() + std::size_t(i) * kRecordStride;
        if (!base) continue;   // (a base without upgrades: none for the new one either)
        std::uint8_t* record = buffer.get() + std::size_t(next) * kRecordStride;
        std::memcpy(record, base, kRecordStride);
        Put<std::uint16_t>(record, kRecordObject, w->zone);
        ++next;
    }
    if (next == count) return;
    std::uint8_t* ours = buffer.get();
    g_buffers.push_back(std::move(buffer));
    Put<std::uint8_t*>(table, kUpgradeRecords, ours);
    Put<std::uint32_t>(table, kUpgradeCount, next);
    g_records = ours;
    g_recordCount = next;
    g_recordGameCount = count;
    Log::Writef("WeaponMods: the weapon upgrade table: %u new weapon(s) with their base's upgrades", next - count);
}

// The upgrades' texts and icons in their traits and recipes (checked again every second: written over when the game's
// came back)
void ApplyUpgradeTexts() {
    if (g_upgrades.empty() || !g_traitsGlobal) return;
    const ULONGLONG now = GetTickCount64();
    if (now - g_lastTexts < 1000) return;
    g_lastTexts = now;
    std::uint8_t* traits = nullptr;
    std::uint8_t* traitList = nullptr;
    std::uint32_t traitCount = 0;
    if (!ReadAt(g_traitsGlobal, traits) || !traits || !ReadAt(reinterpret_cast<std::uintptr_t>(traits + kTraitList), traitList) ||
        !ReadAt(reinterpret_cast<std::uintptr_t>(traits + kTraitCount), traitCount) || !traitList)
        return;
    std::uint8_t* crafting = nullptr;
    std::uint8_t* recipes = nullptr;
    std::uint32_t recipeCount = 0;
    if (ReadAt(GameBuild::Base() + kRvaGlobalCraftingPtr, crafting) && crafting) {
        ReadAt(reinterpret_cast<std::uintptr_t>(crafting + kRecipeList), recipes);
        ReadAt(reinterpret_cast<std::uintptr_t>(crafting + kRecipeCount), recipeCount);
    }
    static bool s_logged = false;
    int written = 0;
    for (const auto& u : g_upgrades) {
        for (const std::uint16_t index : {u.trait, u.previewTrait}) {
            if (index == 0xFFFF || index >= traitCount) continue;
            std::uint8_t* entry = traitList + std::size_t(index) * kTraitStride;
            std::uint8_t copy[kTraitStride];
            if (!CopyGuarded(copy, entry, kTraitStride)) continue;
            if (u.name.id >= 0 && Take<std::int16_t>(copy, kTraitName) != u.name.id) Put<std::int16_t>(entry, kTraitName, std::int16_t(u.name.id)), ++written;
            if (u.description.id >= 0 && Take<std::int16_t>(copy, kTraitDescription) != u.description.id)
                Put<std::int16_t>(entry, kTraitDescription, std::int16_t(u.description.id)), ++written;
            if (!u.icon.empty() && Take<const char*>(copy, kTraitIcon) != StoredIcon(u.icon))
                Put<const char*>(entry, kTraitIcon, StoredIcon(u.icon)), ++written;
            // the recipe the upgrade is crafted with (the crafting panel's title and description)
            const std::uint32_t recipe = Take<std::uint32_t>(copy, kTraitRecipe);
            if (index == u.trait && recipes && recipe < recipeCount) {
                std::uint8_t* r = recipes + std::size_t(recipe) * kRecipeStride;
                std::int16_t name = 0, description = 0;
                if (!ReadAt(reinterpret_cast<std::uintptr_t>(r + kRecipeName), name) ||
                    !ReadAt(reinterpret_cast<std::uintptr_t>(r + kRecipeDescription), description))
                    continue;
                if (u.name.id >= 0 && name != u.name.id) Put<std::int16_t>(r, kRecipeName, std::int16_t(u.name.id)), ++written;
                if (u.description.id >= 0 && description != u.description.id)
                    Put<std::int16_t>(r, kRecipeDescription, std::int16_t(u.description.id)), ++written;
            }
        }
    }
    if (written && !s_logged) Log::Writef("WeaponMods: %zu upgrade(s) of mods have their texts and icons", g_upgrades.size());
    if (written) s_logged = true;
}
}  // namespace

void ParseMod(const Json& mod, const std::string& key, const fs::path& folder) {
    int weapons = 0, added = 0, upgrades = 0, values = 0;
    if (auto list = mod.find("weapons"); list != mod.end() && list->is_array())
        for (const auto& e : *list) {
            if (!e.is_object()) continue;
            WeaponConfig w;
            w.mod = key;
            w.object = Lower(JsonText(e, "object"));
            w.base = Lower(JsonText(e, "base"));
            w.unlock = Lower(JsonText(e, "unlock"));
            w.key = key + "/" + w.object;
            w.name.text = JsonText(e, "name");
            w.description.text = JsonText(e, "description");
            w.icon = JsonText(e, "icon");
            if (auto bars = e.find("bars"); bars != e.end() && bars->is_array() && bars->size() == 6) {
                w.hasBars = true;
                for (std::size_t b = 0; b < 6; ++b) w.bars[b] = (*bars)[b].is_number() ? (*bars)[b].get<float>() : 0.0f;
            }
            if (const std::string sub = JsonText(e, "folder"); !sub.empty() && w.IsNew()) w.folder = folder / PathFromUtf8(sub);
            if (const std::string sub = JsonText(e, "ui_folder"); !sub.empty() && w.IsNew()) {   // (v0.9.569)
                w.uiFolder = folder / PathFromUtf8(sub);
                w.uiBase = Lower(JsonText(e, "ui_base"));
                w.uiObject = w.object + "_ui";
                if (w.uiBase.empty()) w.uiObject.clear();
            }
            if (w.object.empty()) continue;
            if (w.IsNew() && std::any_of(g_weapons.begin(), g_weapons.end(), [&](const WeaponConfig& o) { return o.object == w.object; })) {
                Log::Writef("WeaponMods: %s skipped: another mod already adds %s", w.key.c_str(), w.object.c_str());
                continue;
            }
            added += w.IsNew() ? 1 : 0;
            ++weapons;
            g_weapons.push_back(std::move(w));
        }
    if (auto list = mod.find("upgrades"); list != mod.end() && list->is_array())
        for (const auto& e : *list) {
            if (!e.is_object()) continue;
            UpgradeConfig u;
            u.mod = key;
            u.upgrade = JsonNumber(e, "upgrade", 0);
            u.trait = static_cast<std::uint16_t>(JsonNumber(e, "trait", 0xFFFF));
            u.previewTrait = static_cast<std::uint16_t>(JsonNumber(e, "preview_trait", 0xFFFF));
            u.name.text = JsonText(e, "name");
            u.description.text = JsonText(e, "description");
            u.icon = JsonText(e, "icon");
            if (u.trait == 0xFFFF) continue;
            ++upgrades;
            g_upgrades.push_back(std::move(u));
        }
    if (auto list = mod.find("values"); list != mod.end() && list->is_array())
        for (const auto& e : *list) {
            if (!e.is_object()) continue;
            ValueConfig v;
            v.mod = key;
            v.dtp = JsonNumber(e, "dtp", 0);
            v.offset = JsonNumber(e, "offset", 0);
            v.size = JsonNumber(e, "size", 0);
            v.type = JsonText(e, "type");
            v.value = JsonText(e, "value");
            v.original = JsonText(e, "original");
            v.label = JsonText(e, "label");
            if (!v.dtp || v.type.empty() || v.value.empty()) continue;
            ++values;
            g_values.push_back(std::move(v));
        }
    if (weapons || upgrades || values) {
        // (the Modding Suite looks for this text in the .asi to tell a framework that reads them)
        Log::Writef("WeaponMods: mod %s: mod.json weapons, upgrades and values: %d weapon(s) (%d new), %d upgrade(s), %d value(s)",
                    key.c_str(), weapons, added, upgrades, values);
        g_status = "configured";
    }
}

bool Any() { return !g_weapons.empty() || !g_upgrades.empty() || !g_values.empty(); }

std::vector<Text*> Texts() {
    std::vector<Text*> out;
    for (auto& w : g_weapons) {
        if (!w.name.text.empty()) out.push_back(&w.name);
        if (!w.description.text.empty()) out.push_back(&w.description);
    }
    for (auto& u : g_upgrades) {
        if (!u.name.text.empty()) out.push_back(&u.name);
        if (!u.description.text.empty()) out.push_back(&u.description);
    }
    return out;
}

void AddSpecs(std::vector<OutfitArchive::OutfitSpec>& specs) {
    for (const auto& w : g_weapons) {
        if (!w.IsNew()) continue;
        if (std::any_of(specs.begin(), specs.end(), [&](const OutfitArchive::OutfitSpec& s) { return s.objectName == w.object; })) {
            Log::Writef("WeaponMods: %s skipped: an outfit or model already builds %s", w.key.c_str(), w.object.c_str());
            continue;
        }
        OutfitArchive::OutfitSpec spec;
        spec.key = w.key;
        spec.objectName = w.object;
        spec.baseObjectName = w.base;
        if (FolderHasResources(w.folder)) spec.folder = w.folder;
        else spec.cloneOnly = true;
        Log::Detailf("WeaponMods: %s: a new weapon object %s, built from %s%s", w.key.c_str(), w.object.c_str(), w.base.c_str(),
                    spec.cloneOnly ? " (a copy: no files of its own)" : "");
        specs.push_back(std::move(spec));
        // (v0.9.569) its campsite model
        if (w.uiObject.empty() ||
            std::any_of(specs.begin(), specs.end(), [&](const OutfitArchive::OutfitSpec& s) { return s.objectName == w.uiObject; }))
            continue;
        OutfitArchive::OutfitSpec ui;
        ui.key = w.key + "/campsite";
        ui.objectName = w.uiObject;
        ui.baseObjectName = w.uiBase;
        if (FolderHasResources(w.uiFolder)) ui.folder = w.uiFolder;
        else ui.cloneOnly = true;
        Log::Detailf("WeaponMods: %s: its campsite model %s, built from %s", w.key.c_str(), w.uiObject.c_str(), w.uiBase.c_str());
        specs.push_back(std::move(ui));
    }
}

std::vector<std::pair<std::uint16_t, std::uint16_t>> Aliases() {
    std::vector<std::pair<std::uint16_t, std::uint16_t>> out;
    for (const auto& w : g_weapons) {
        if (w.IsNew() && w.zone && w.baseZone && w.zone != w.baseZone) out.push_back({w.zone, w.baseZone});
        if (w.IsNew() && w.uiZone && w.uiBaseZone && w.uiZone != w.uiBaseZone) out.push_back({w.uiZone, w.uiBaseZone});
    }
    return out;
}

void Tick(const std::function<std::uint16_t(const std::string&)>& findZone) {
    if (!Any()) return;
    CheckGlobals();
    Resolve(findZone);
    ApplyValues();
    ApplyWeaponList();
    ApplyUpgradeTable();
    ApplyUpgradeTexts();
    if (g_list || g_valuesSet || g_records) g_status = "active";
}

const char* Status() { return g_status.c_str(); }
}  // namespace WeaponMods
