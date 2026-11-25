#include "script_engine.h"
#include "features.h"
#include "custom_outfits.h"
#include "log.h"

#include <Windows.h>
#include <imgui.h>

extern "C" {
#include <lua.h>
#include <lauxlib.h>
#include <lualib.h>
}

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cwchar>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace {
namespace fs = std::filesystem;

struct ScriptTab {
    std::string name;
    int callbackRef{LUA_NOREF};
    bool failed{};
};

struct Script {
    lua_State* state{};
    fs::path sourcePath;
    fs::path settingsPath;
    std::string name;
    std::vector<ScriptTab> menuTabs;
    std::vector<int> updateCallbacks;
    std::vector<int> unloadCallbacks;
    std::string lastError;
    bool initialized{};
};

std::vector<std::unique_ptr<Script>> g_scripts;
fs::path g_scriptsDirectory;
fs::path g_settingsDirectory;
std::atomic_bool g_reloadRequested{false};
bool g_initialized = false;
ULONGLONG g_lastTickMs = 0;
std::string g_status = "Lua scripting not initialized";
bool g_uiApiActive = false;

constexpr const char* kScriptRegistryKey = "ROTTRFramework.Script";

Script* CurrentScript(lua_State* L) {
    lua_getfield(L, LUA_REGISTRYINDEX, kScriptRegistryKey);
    auto* script = static_cast<Script*>(lua_touserdata(L, -1));
    lua_pop(L, 1);
    return script;
}

void SetCurrentScript(lua_State* L, Script* script) {
    lua_pushlightuserdata(L, script);
    lua_setfield(L, LUA_REGISTRYINDEX, kScriptRegistryKey);
}

std::wstring Utf8ToWide(std::string_view text) {
    if (text.empty()) return {};
    const int required = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                                              text.data(), static_cast<int>(text.size()),
                                              nullptr, 0);
    if (required <= 0) return {};
    std::wstring result(static_cast<std::size_t>(required), L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                        text.data(), static_cast<int>(text.size()),
                        result.data(), required);
    return result;
}

std::string LuaErrorText(lua_State* L) {
    size_t len = 0;
    const char* text = lua_tolstring(L, -1, &len);
    return text ? std::string(text, len) : std::string("unknown Lua error");
}

void RecordScriptError(Script& script, const char* phase) {
    script.lastError = LuaErrorText(script.state);
    Log::Writef("Lua [%s] %s: %s", script.name.c_str(), phase ? phase : "error",
                script.lastError.c_str());
    lua_pop(script.state, 1);
}

bool CallReference(Script& script, int ref, int nargs = 0, int nresults = 0) {
    if (!script.state || ref == LUA_NOREF || ref == LUA_REFNIL) return false;
    lua_rawgeti(script.state, LUA_REGISTRYINDEX, ref);
    if (!lua_isfunction(script.state, -1)) {
        lua_pop(script.state, 1);
        return false;
    }
    if (nargs > 0)
        lua_insert(script.state, -(nargs + 1));
    if (lua_pcall(script.state, nargs, nresults, 0) != LUA_OK) {
        RecordScriptError(script, "callback failed");
        return false;
    }
    return true;
}

fs::path GameRoot() {
    wchar_t path[MAX_PATH]{};
    const DWORD len = GetModuleFileNameW(nullptr, path, MAX_PATH);
    if (!len || len >= MAX_PATH) return fs::current_path();
    fs::path exe(path);
    return exe.parent_path();
}


bool RequireUiApi(lua_State* L) {
    if (g_uiApiActive) return true;
    luaL_error(L, "ui functions may only be called from a registered Menu tab callback");
    return false;
}
std::wstring SettingKey(lua_State* L, int index) {
    const char* key = luaL_checkstring(L, index);
    return Utf8ToWide(key ? key : "");
}

int LuaFrameworkLog(lua_State* L) {
    Script* script = CurrentScript(L);
    const char* text = luaL_checkstring(L, 1);
    Log::Writef("Lua [%s]: %s", script ? script->name.c_str() : "?", text ? text : "");
    return 0;
}

int LuaFrameworkRegisterMenuTab(lua_State* L) {
    Script* script = CurrentScript(L);
    if (!script) return luaL_error(L, "script context is unavailable");
    if (script->initialized) return luaL_error(L, "callbacks and Menu tabs must be registered during script initialization");
    const char* name = luaL_checkstring(L, 1);
    luaL_checktype(L, 2, LUA_TFUNCTION);
    lua_pushvalue(L, 2);
    const int ref = luaL_ref(L, LUA_REGISTRYINDEX);
    script->menuTabs.push_back({name ? name : "Script", ref});
    return 0;
}

int LuaFrameworkOnUpdate(lua_State* L) {
    Script* script = CurrentScript(L);
    if (!script) return luaL_error(L, "script context is unavailable");
    if (script->initialized) return luaL_error(L, "callbacks and Menu tabs must be registered during script initialization");
    luaL_checktype(L, 1, LUA_TFUNCTION);
    lua_pushvalue(L, 1);
    script->updateCallbacks.push_back(luaL_ref(L, LUA_REGISTRYINDEX));
    return 0;
}

int LuaFrameworkOnUnload(lua_State* L) {
    Script* script = CurrentScript(L);
    if (!script) return luaL_error(L, "script context is unavailable");
    if (script->initialized) return luaL_error(L, "callbacks and Menu tabs must be registered during script initialization");
    luaL_checktype(L, 1, LUA_TFUNCTION);
    lua_pushvalue(L, 1);
    script->unloadCallbacks.push_back(luaL_ref(L, LUA_REGISTRYINDEX));
    return 0;
}

int LuaFrameworkReload(lua_State*) {
    g_reloadRequested.store(true, std::memory_order_release);
    return 0;
}

int LuaSettingsGetBool(lua_State* L) {
    Script* script = CurrentScript(L);
    if (!script) return luaL_error(L, "script context is unavailable");
    const std::wstring key = SettingKey(L, 1);
    const bool fallback = lua_gettop(L) >= 2 ? lua_toboolean(L, 2) != 0 : false;
    const int value = GetPrivateProfileIntW(L"Settings", key.c_str(), fallback ? 1 : 0,
                                             script->settingsPath.c_str());
    lua_pushboolean(L, value != 0);
    return 1;
}

int LuaSettingsSetBool(lua_State* L) {
    Script* script = CurrentScript(L);
    if (!script) return luaL_error(L, "script context is unavailable");
    const std::wstring key = SettingKey(L, 1);
    const bool value = lua_toboolean(L, 2) != 0;
    WritePrivateProfileStringW(L"Settings", key.c_str(), value ? L"1" : L"0",
                               script->settingsPath.c_str());
    return 0;
}

int LuaSettingsGetNumber(lua_State* L) {
    Script* script = CurrentScript(L);
    if (!script) return luaL_error(L, "script context is unavailable");
    const std::wstring key = SettingKey(L, 1);
    const double fallback = lua_gettop(L) >= 2 ? luaL_checknumber(L, 2) : 0.0;
    wchar_t defaultText[64]{};
    wchar_t valueText[64]{};
    swprintf_s(defaultText, L"%.17g", fallback);
    GetPrivateProfileStringW(L"Settings", key.c_str(), defaultText, valueText,
                             static_cast<DWORD>(std::size(valueText)), script->settingsPath.c_str());
    wchar_t* end = nullptr;
    const double value = wcstod(valueText, &end);
    lua_pushnumber(L, (end && end != valueText && std::isfinite(value)) ? value : fallback);
    return 1;
}

int LuaSettingsSetNumber(lua_State* L) {
    Script* script = CurrentScript(L);
    if (!script) return luaL_error(L, "script context is unavailable");
    const std::wstring key = SettingKey(L, 1);
    const double value = luaL_checknumber(L, 2);
    if (!std::isfinite(value)) return luaL_error(L, "setting value must be finite");
    wchar_t text[64]{};
    swprintf_s(text, L"%.17g", value);
    WritePrivateProfileStringW(L"Settings", key.c_str(), text, script->settingsPath.c_str());
    return 0;
}

int LuaUiCheckbox(lua_State* L) {
    if (!RequireUiApi(L)) return 0;
    const char* label = luaL_checkstring(L, 1);
    bool value = lua_toboolean(L, 2) != 0;
    const bool disabled = lua_gettop(L) >= 3 && lua_toboolean(L, 3) != 0;
    if (disabled) ImGui::BeginDisabled();
    const bool changed = ImGui::Checkbox(label, &value);
    if (disabled) ImGui::EndDisabled();
    lua_pushboolean(L, changed);
    lua_pushboolean(L, value);
    return 2;
}

int LuaUiInputFloat(lua_State* L) {
    if (!RequireUiApi(L)) return 0;
    const char* label = luaL_checkstring(L, 1);
    float value = static_cast<float>(luaL_checknumber(L, 2));
    const float step = static_cast<float>(luaL_optnumber(L, 3, 0.0));
    const float stepFast = static_cast<float>(luaL_optnumber(L, 4, 0.0));
    const char* format = luaL_optstring(L, 5, "%.3f");
    const bool disabled = lua_gettop(L) >= 6 && lua_toboolean(L, 6) != 0;
    if (disabled) ImGui::BeginDisabled();
    const bool changed = ImGui::InputFloat(label, &value, step, stepFast, format);
    if (disabled) ImGui::EndDisabled();
    lua_pushboolean(L, changed);
    lua_pushnumber(L, value);
    return 2;
}

int LuaUiInputText(lua_State* L) {
    if (!RequireUiApi(L)) return 0;
    const char* label = luaL_checkstring(L, 1);
    const char* value = luaL_optstring(L, 2, "");
    int capacity = static_cast<int>(luaL_optinteger(L, 3, 64));
    const bool disabled = lua_gettop(L) >= 4 && lua_toboolean(L, 4) != 0;
    capacity = std::clamp(capacity, 2, 255);

    char buffer[256]{};
    strncpy_s(buffer, value ? value : "", static_cast<std::size_t>(capacity - 1));
    if (disabled) ImGui::BeginDisabled();
    const bool changed = ImGui::InputText(label, buffer, static_cast<std::size_t>(capacity));
    if (disabled) ImGui::EndDisabled();
    lua_pushboolean(L, changed);
    lua_pushstring(L, buffer);
    return 2;
}

int LuaUiButton(lua_State* L) {
    if (!RequireUiApi(L)) return 0;
    const char* label = luaL_checkstring(L, 1);
    const bool disabled = lua_gettop(L) >= 2 && lua_toboolean(L, 2) != 0;
    if (disabled) ImGui::BeginDisabled();
    const bool pressed = ImGui::Button(label);
    if (disabled) ImGui::EndDisabled();
    lua_pushboolean(L, pressed);
    return 1;
}

int LuaUiSameLine(lua_State* L) {
    if (!RequireUiApi(L)) return 0;
    const float offset = static_cast<float>(luaL_optnumber(L, 1, 0.0));
    const float spacing = static_cast<float>(luaL_optnumber(L, 2, -1.0));
    ImGui::SameLine(offset, spacing);
    return 0;
}

int LuaUiSetNextItemWidth(lua_State* L) {
    if (!RequireUiApi(L)) return 0;
    const float width = static_cast<float>(luaL_checknumber(L, 1));
    ImGui::SetNextItemWidth(width);
    return 0;
}

int LuaUiText(lua_State* L) {
    if (!RequireUiApi(L)) return 0;
    ImGui::TextUnformatted(luaL_checkstring(L, 1));
    return 0;
}

int LuaUiTextDisabled(lua_State* L) {
    if (!RequireUiApi(L)) return 0;
    ImGui::TextDisabled("%s", luaL_checkstring(L, 1));
    return 0;
}

int LuaUiTextWrapped(lua_State* L) {
    if (!RequireUiApi(L)) return 0;
    ImGui::TextWrapped("%s", luaL_checkstring(L, 1));
    return 0;
}

int LuaUiSeparator(lua_State* L) {
    if (!RequireUiApi(L)) return 0;
    ImGui::Separator();
    return 0;
}

int LuaUiSpacing(lua_State* L) {
    if (!RequireUiApi(L)) return 0;
    ImGui::Spacing();
    return 0;
}

int LuaWetAvailable(lua_State* L) {
    lua_pushboolean(L, Features::WetMaterialTransitionAvailable());
    return 1;
}

int LuaWetEnabled(lua_State* L) {
    lua_pushboolean(L, Features::WetMaterialTransitionEnabled());
    return 1;
}

int LuaWetSetEnabled(lua_State* L) {
    Features::SetWetMaterialTransitionEnabled(lua_toboolean(L, 1) != 0);
    return 0;
}

int LuaWetWetIn(lua_State* L) {
    lua_pushnumber(L, Features::WetMaterialWetInSeconds());
    return 1;
}

int LuaWetSetWetIn(lua_State* L) {
    Features::SetWetMaterialWetInSeconds(static_cast<float>(luaL_checknumber(L, 1)));
    return 0;
}

int LuaWetDryOut(lua_State* L) {
    lua_pushnumber(L, Features::WetMaterialDryOutSeconds());
    return 1;
}

int LuaWetSetDryOut(lua_State* L) {
    Features::SetWetMaterialDryOutSeconds(static_cast<float>(luaL_checknumber(L, 1)));
    return 0;
}

int LuaWetStatus(lua_State* L) {
    lua_pushstring(L, Features::WetMaterialTransitionStatus());
    return 1;
}


bool CheckMaterialResourceId(lua_State* L, int index, std::uint32_t& out) {
    const lua_Integer value = luaL_checkinteger(L, index);
    if (value <= 0 || static_cast<unsigned long long>(value) > 0xFFFFFFFFull) {
        luaL_error(L, "material resource ID must be between 1 and 4294967295");
        return false;
    }
    out = static_cast<std::uint32_t>(value);
    return true;
}

int LuaMaterialVisibilityAvailable(lua_State* L) {
    lua_pushboolean(L, Features::MaterialResourceVisibilityAvailable());
    return 1;
}

int LuaMaterialSetHidden(lua_State* L) {
    std::uint32_t resourceId{};
    if (!CheckMaterialResourceId(L, 1, resourceId)) return 0;
    const bool hidden = lua_toboolean(L, 2) != 0;
    lua_pushboolean(L, Features::SetMaterialResourceHidden(resourceId, hidden));
    return 1;
}

int LuaMaterialToggleHidden(lua_State* L) {
    std::uint32_t resourceId{};
    if (!CheckMaterialResourceId(L, 1, resourceId)) return 0;
    bool hidden = false;
    const bool ok = Features::ToggleMaterialResourceHidden(resourceId, hidden);
    lua_pushboolean(L, ok);
    lua_pushboolean(L, hidden);
    return 2;
}

int LuaMaterialIsHidden(lua_State* L) {
    std::uint32_t resourceId{};
    if (!CheckMaterialResourceId(L, 1, resourceId)) return 0;
    lua_pushboolean(L, Features::IsMaterialResourceHidden(resourceId));
    return 1;
}

int LuaMaterialSuppressedDrawCount(lua_State* L) {
    lua_pushinteger(L, static_cast<lua_Integer>(Features::MaterialResourceSuppressedDrawCount()));
    return 1;
}

int LuaMarkerAvailable(lua_State* L) {
    lua_pushboolean(L, Features::PlayerMarkerOverrideAvailable());
    return 1;
}

int LuaMarkerSetPositionOverride(lua_State* L) {
    const char* name = luaL_checkstring(L, 1);
    const float x = static_cast<float>(luaL_checknumber(L, 2));
    const float y = static_cast<float>(luaL_checknumber(L, 3));
    const float z = static_cast<float>(luaL_checknumber(L, 4));
    lua_pushboolean(L, Features::SetPlayerMarkerPositionOverride(name, x, y, z));
    return 1;
}

int LuaMarkerClearPositionOverride(lua_State* L) {
    const char* name = luaL_checkstring(L, 1);
    lua_pushboolean(L, Features::ClearPlayerMarkerPositionOverride(name));
    return 1;
}

void SetFunction(lua_State* L, const char* name, lua_CFunction fn) {
    lua_pushcfunction(L, fn);
    lua_setfield(L, -2, name);
}

void InstallApi(lua_State* L) {
    
    
    
    luaL_openlibs(L);
    for (const char* global : {"io", "os", "debug", "package", "dofile", "loadfile", "require"}) {
        lua_pushnil(L);
        lua_setglobal(L, global);
    }
    lua_pushcfunction(L, LuaFrameworkLog);
    lua_setglobal(L, "print");

    lua_newtable(L); 
    lua_pushstring(L, "0.9.180");
    lua_setfield(L, -2, "version");
    lua_pushinteger(L, 3);
    lua_setfield(L, -2, "api_version");
    SetFunction(L, "log", LuaFrameworkLog);
    SetFunction(L, "register_menu_tab", LuaFrameworkRegisterMenuTab);
    SetFunction(L, "on_update", LuaFrameworkOnUpdate);
    SetFunction(L, "on_unload", LuaFrameworkOnUnload);
    SetFunction(L, "reload", LuaFrameworkReload);

    lua_newtable(L); 
    SetFunction(L, "get_bool", LuaSettingsGetBool);
    SetFunction(L, "set_bool", LuaSettingsSetBool);
    SetFunction(L, "get_number", LuaSettingsGetNumber);
    SetFunction(L, "set_number", LuaSettingsSetNumber);
    lua_setfield(L, -2, "settings");
    lua_setglobal(L, "framework");

    lua_newtable(L); 
    SetFunction(L, "checkbox", LuaUiCheckbox);
    SetFunction(L, "input_float", LuaUiInputFloat);
    SetFunction(L, "input_text", LuaUiInputText);
    SetFunction(L, "button", LuaUiButton);
    SetFunction(L, "same_line", LuaUiSameLine);
    SetFunction(L, "set_next_item_width", LuaUiSetNextItemWidth);
    SetFunction(L, "text", LuaUiText);
    SetFunction(L, "text_disabled", LuaUiTextDisabled);
    SetFunction(L, "text_wrapped", LuaUiTextWrapped);
    SetFunction(L, "separator", LuaUiSeparator);
    SetFunction(L, "spacing", LuaUiSpacing);
    lua_setglobal(L, "ui");

    lua_newtable(L); 
    lua_newtable(L); 
    SetFunction(L, "available", LuaWetAvailable);
    SetFunction(L, "enabled", LuaWetEnabled);
    SetFunction(L, "set_enabled", LuaWetSetEnabled);
    SetFunction(L, "wet_in_seconds", LuaWetWetIn);
    SetFunction(L, "set_wet_in_seconds", LuaWetSetWetIn);
    SetFunction(L, "dry_out_seconds", LuaWetDryOut);
    SetFunction(L, "set_dry_out_seconds", LuaWetSetDryOut);
    SetFunction(L, "status", LuaWetStatus);
    lua_setfield(L, -2, "wet");

    lua_newtable(L); 
    SetFunction(L, "available", LuaMaterialVisibilityAvailable);
    SetFunction(L, "set_hidden", LuaMaterialSetHidden);
    SetFunction(L, "toggle_hidden", LuaMaterialToggleHidden);
    SetFunction(L, "is_hidden", LuaMaterialIsHidden);
    SetFunction(L, "suppressed_draw_count", LuaMaterialSuppressedDrawCount);
    lua_setfield(L, -2, "material");

    lua_newtable(L); 
    SetFunction(L, "available", LuaMarkerAvailable);
    SetFunction(L, "set_position_override", LuaMarkerSetPositionOverride);
    SetFunction(L, "clear_position_override", LuaMarkerClearPositionOverride);
    lua_setfield(L, -2, "marker");

    lua_setglobal(L, "rottr");
}

void UnloadScript(Script& script) {
    if (!script.state) return;
    for (const int ref : script.unloadCallbacks) {
        if (!CallReference(script, ref))
            break;
    }
    lua_close(script.state);
    script.state = nullptr;
}

void UnloadAll() {
    for (auto& script : g_scripts) {
        if (script) UnloadScript(*script);
    }
    g_scripts.clear();
}

// outfit: the custom outfit (folder or package) the script comes with, empty for ROTTRFramework\*.lua. Its settings
// file is settings\<outfit>.<script>.ini.
bool LoadScript(const fs::path& sourcePath, const std::string& outfit = {}) {
    auto script = std::make_unique<Script>();
    script->sourcePath = sourcePath;
    script->name = outfit.empty() ? sourcePath.stem().string() : outfit + "/" + sourcePath.stem().string();
    script->settingsPath = g_settingsDirectory / (outfit.empty() ? sourcePath.stem().wstring() + L".ini"
                                                                 : fs::path(outfit).wstring() + L"." +
                                                                       sourcePath.stem().wstring() + L".ini");
    script->state = luaL_newstate();
    if (!script->state) {
        Log::Writef("Lua [%s]: could not create state", script->name.c_str());
        return false;
    }

    SetCurrentScript(script->state, script.get());
    InstallApi(script->state);

    std::ifstream file(sourcePath, std::ios::binary);
    if (!file) {
        Log::Writef("Lua [%s]: could not open script file", script->name.c_str());
        lua_close(script->state);
        return false;
    }
    std::string source((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    const std::string chunkName = "@" + sourcePath.filename().string();
    if (luaL_loadbufferx(script->state, source.data(), source.size(), chunkName.c_str(), "t") != LUA_OK) {
        RecordScriptError(*script, "load failed");
        lua_close(script->state);
        return false;
    }
    if (lua_pcall(script->state, 0, 0, 0) != LUA_OK) {
        RecordScriptError(*script, "initialization failed");
        lua_close(script->state);
        return false;
    }
    script->initialized = true;

    Log::Detailf("Lua [%s]: loaded (%zu menu tabs, %zu update callbacks)",
                script->name.c_str(), script->menuTabs.size(), script->updateCallbacks.size());
    g_scripts.push_back(std::move(script));
    return true;
}

int LoadAll() {
    std::error_code ec;
    fs::create_directories(g_scriptsDirectory, ec);
    ec.clear();
    fs::create_directories(g_settingsDirectory, ec);

    auto luaFiles = [](const fs::path& folder) {
        std::vector<fs::path> found;
        std::error_code error;
        for (fs::directory_iterator it(folder, error), end; !error && it != end; it.increment(error)) {
            if (!it->is_regular_file(error)) continue;
            fs::path path = it->path();
            std::wstring ext = path.extension().wstring();
            std::transform(ext.begin(), ext.end(), ext.begin(), [](wchar_t c) {
                return static_cast<wchar_t>(towlower(c));
            });
            if (ext == L".lua") found.push_back(std::move(path));
        }
        std::sort(found.begin(), found.end());
        return found;
    };
    std::vector<std::pair<fs::path, std::string>> sources;
    for (auto& path : luaFiles(g_scriptsDirectory)) sources.push_back({std::move(path), std::string()});
    // Scripts that come with a custom outfit (next to its outfit.ini, also in .zip packages).
    for (const auto& outfit : CustomOutfits::ScriptFolders())
        for (auto& path : luaFiles(outfit.folder)) sources.push_back({std::move(path), outfit.outfit});

    int loaded = 0;
    for (const auto& source : sources)
        loaded += LoadScript(source.first, source.second) ? 1 : 0;

    char status[192]{};
    sprintf_s(status, "Lua 5.4: %d/%zu script%s loaded from ROTTRFramework",
              loaded, sources.size(), sources.size() == 1 ? "" : "s");
    g_status = status;
    Log::Detail(g_status);
    return loaded;
}

void ReloadAll() {
    Log::Detail("Lua: reloading scripts");
    UnloadAll();
    LoadAll();
}
} 

namespace ScriptEngine {
bool Initialize() {
    if (g_initialized) return true;
    g_scriptsDirectory = GameRoot() / L"ROTTRFramework";
    g_settingsDirectory = g_scriptsDirectory / L"settings";
    g_lastTickMs = GetTickCount64();
    LoadAll();
    g_initialized = true;
    return true;
}

void Shutdown() {
    if (!g_initialized) return;
    UnloadAll();
    g_initialized = false;
}

void Tick() {
    if (!g_initialized) return;
    if (g_reloadRequested.exchange(false, std::memory_order_acq_rel))
        ReloadAll();

    const ULONGLONG now = GetTickCount64();
    float dt = 0.0f;
    if (g_lastTickMs && now >= g_lastTickMs)
        dt = static_cast<float>(now - g_lastTickMs) / 1000.0f;
    g_lastTickMs = now;
    dt = std::clamp(dt, 0.0f, 0.25f);

    for (auto& owned : g_scripts) {
        if (!owned || !owned->state) continue;
        Script& script = *owned;
        for (int& ref : script.updateCallbacks) {
            if (ref == LUA_NOREF || ref == LUA_REFNIL) continue;
            lua_pushnumber(script.state, dt);
            if (!CallReference(script, ref, 1, 0)) {
                luaL_unref(script.state, LUA_REGISTRYINDEX, ref);
                ref = LUA_NOREF;
                break;
            }
        }
    }
}

void RequestReload() {
    g_reloadRequested.store(true, std::memory_order_release);
}

void DrawMenuTabs() {
    if (!g_initialized) return;
    for (auto& owned : g_scripts) {
        if (!owned || !owned->state) continue;
        Script& script = *owned;
        for (std::size_t tabIndex = 0; tabIndex < script.menuTabs.size(); ++tabIndex) {
            auto& tab = script.menuTabs[tabIndex];
            const std::string tabId = tab.name + "##lua_" + script.name + "_" + std::to_string(tabIndex);
            if (!ImGui::BeginTabItem(tabId.c_str())) continue;
            if (tab.failed) {
                ImGui::TextWrapped("Lua error: %s", script.lastError.c_str());
                ImGui::TextDisabled("Reload scripts after correcting the error.");
            } else {
                g_uiApiActive = true;
                if (!CallReference(script, tab.callbackRef))
                    tab.failed = true;
                g_uiApiActive = false;
            }
            ImGui::EndTabItem();
        }
    }
}

int LoadedScriptCount() {
    return static_cast<int>(g_scripts.size());
}

const char* Status() {
    return g_status.c_str();
}
} 
