#include "settings.h"
#include <Windows.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <cmath>
#include <initializer_list>

namespace {
// (v0.9.459) [Settings] Version written by Save: this framework version's last part (0.9.459 -> 459).
constexpr int kIniVersion = 459;
Settings::State g_state;
char g_iniPath[MAX_PATH]{};
const char* IniPath() {
    if (g_iniPath[0]) return g_iniPath;

    char root[MAX_PATH]{};
    GetModuleFileNameA(nullptr, root, MAX_PATH);
    char* slash = root;
    for (char* p = root; *p; ++p) if (*p == '\\' || *p == '/') slash = p + 1;
    *slash = '\0';

    strcpy_s(g_iniPath, root);
    lstrcatA(g_iniPath, "ROTTRFramework.ini");

    
    
    
    if (GetFileAttributesA(g_iniPath) == INVALID_FILE_ATTRIBUTES) {
        char legacy[MAX_PATH]{};
        strcpy_s(legacy, root);
        lstrcatA(legacy, "rottr_trainer.ini");
        if (GetFileAttributesA(legacy) != INVALID_FILE_ATTRIBUTES)
            CopyFileA(legacy, g_iniPath, TRUE);
    }
    return g_iniPath;
}

float ReadFloat(const char* section, const char* key, float fallback, const char* path) {
    char def[64]{};
    sprintf_s(def, "%.6f", fallback);
    char buf[64]{};
    GetPrivateProfileStringA(section, key, def, buf, sizeof(buf), path);
    return static_cast<float>(std::atof(buf));
}

bool ReadBool(const char* section, const char* key, bool fallback, const char* path) {
    return GetPrivateProfileIntA(section, key, fallback ? 1 : 0, path) != 0;
}

void WriteBool(const char* section, const char* key, bool value, const char* path) {
    WritePrivateProfileStringA(section, key, value ? "1" : "0", path);
}

void WriteFloat(const char* section, const char* key, float value, const char* path) {
    char buf[64]{};
    sprintf_s(buf, "%.7g", value);
    WritePrivateProfileStringA(section, key, buf, path);
}

void WriteInt(const char* section, const char* key, int value, const char* path) {
    char buf[32]{};
    sprintf_s(buf, "%d", value);
    WritePrivateProfileStringA(section, key, buf, path);
}

void ReadString(const char* section, const char* key, const char* fallback,
                char* out, unsigned int outSize, const char* path) {
    if (!out || outSize == 0) return;
    GetPrivateProfileStringA(section, key, fallback ? fallback : "", out, outSize, path);
}

void WriteString(const char* section, const char* key, const char* value, const char* path) {
    WritePrivateProfileStringA(section, key, value ? value : "", path);
}
}

namespace Settings {
State& Get() { return g_state; }

void Load() {
    const char* path = IniPath();
    g_state.uiScale = ReadFloat("UI", "Scale", 1.0f, path);
    {   // (v1.0.0) no Scale key: the first run (overlay.cpp chooses one from the resolution)
        char text[32]{};
        ReadString("UI", "Scale", "", text, sizeof(text), path);
        g_state.uiScaleSaved = text[0] != '\0';
    }
    g_state.uiScale = std::clamp(g_state.uiScale, 1.0f, 2.5f);
    g_state.uiTheme = static_cast<int>(std::clamp(GetPrivateProfileIntA("UI", "Theme", 0, path), 0u, 2u));

    g_state.fovMultiplier = ReadFloat("Camera", "FovMultiplier", 1.0f, path);
    if (g_state.fovMultiplier < 0.25f || g_state.fovMultiplier > 4.0f)
        g_state.fovMultiplier = 1.0f;

    g_state.deathReloadDelay = ReadFloat("Player", "DeathReloadDelay", 0.0f, path);
    g_state.deathReloadDelay = std::clamp(g_state.deathReloadDelay, 0.0f, 60.0f);

    g_state.drawInstancesDistance = ReadFloat("Draw", "InstancesDistance", 1500.0f, path);
    g_state.drawInstancesDistance = std::clamp(g_state.drawInstancesDistance, 0.0f, 10000.0f);

    // (v0.9.597) Draw collision removed: its distance (which Draw markup used) is the markup's own now
    g_state.drawMarkupDistance =
        ReadFloat("Draw", "MarkupDistance", ReadFloat("Draw", "CollisionDistance", 1500.0f, path), path);
    g_state.drawMarkupDistance = std::clamp(g_state.drawMarkupDistance, 0.0f, 10000.0f);

    // (v0.9.607) Draw wireframe's options
    g_state.wireframeStyle = GetPrivateProfileIntA("Draw", "WireframeStyle", 0, path) == 1 ? 1 : 0;
    {
        char text[96]{};
        float c[4]{};
        ReadString("Draw", "WireframeColor", "", text, sizeof(text), path);
        if (sscanf_s(text, "%f %f %f %f", &c[0], &c[1], &c[2], &c[3]) == 4)
            for (int i = 0; i < 4; ++i)
                if (std::isfinite(c[i])) g_state.wireframeColor[i] = std::clamp(c[i], 0.0f, i == 3 ? 1.0f : 64.0f);
        ReadString("Draw", "WireframeBackground", "", text, sizeof(text), path);
        if (sscanf_s(text, "%f %f %f", &c[0], &c[1], &c[2]) == 3)
            for (int i = 0; i < 3; ++i)
                if (std::isfinite(c[i])) g_state.wireframeBackground[i] = std::clamp(c[i], 0.0f, 64.0f);
    }
    g_state.wireframeSeeThrough = ReadBool("Draw", "WireframeSeeThrough", false, path);
    g_state.wireframeBackFaces = ReadBool("Draw", "WireframeBackFaces", false, path);

    // (v0.9.459) The ini's version: [Settings] Version, the framework version that last saved it (459 for 0.9.459;
    // missing before). Earlier versions saved every option, so their DoNotPauseOnFocusLoss=0 was the old default
    // rather than a choice: an ini from before 0.9.459 starts with it on.
    const int iniVersion = static_cast<int>(GetPrivateProfileIntA("Settings", "Version", 0, path));
    g_state.fasterLoading = ReadBool("Loading", "FasterLoading", true, path);
    g_state.doNotPauseOnFocusLoss = iniVersion < kIniVersion
        ? true : ReadBool("General", "DoNotPauseOnFocusLoss", true, path);
    g_state.allowCampsitesNearEnemies = ReadBool("General", "AllowCampsitesNearEnemies", false, path);
    g_state.hideHints = ReadBool("Gameplay", "HideHints", false, path);                  // (v0.9.586)
    g_state.noDrawOnWeaponSwitch = ReadBool("Gameplay", "NoDrawOnSwitch", false, path);
    g_state.hideLowerGear = ReadBool("Gameplay", "HideLowerGear", false, path);
    g_state.hideUpperGear = ReadBool("Gameplay", "HideUpperGear", false, path);
    g_state.hideBodyGear = ReadBool("Gameplay", "HideBodyGear", false, path);
    g_state.hideQuiverStrap = ReadBool("Gameplay", "HideQuiverStrap", false, path);
    g_state.weaponSelectBottomLeft = ReadBool("Gameplay", "WeaponSelectBottomLeft", false, path);   // (v0.9.589)
    g_state.weaponSelectSize = std::clamp(static_cast<int>(GetPrivateProfileIntA("Gameplay", "WeaponSelectSize", 65, path)),
                                          30, 100);   // (v0.9.590)
    g_state.noCutsceneOutfitChanges = ReadBool("Outfits", "NoCutsceneOutfitChanges", false, path);
    g_state.clothLevelCollision = ReadBool("General", "ClothLevelCollision", false, path);
    g_state.fixCutsceneBonePositions = ReadBool("General", "FixCutsceneBonePositions", false, path);
    g_state.dualWieldPistols = ReadBool("Weapons", "DualWieldPistols", true, path);
    g_state.silencerToggle = ReadBool("Weapons", "SilencerToggle", true, path);
    g_state.silencerNoReload = ReadBool("Weapons", "SilencerNoReload", true, path);   // (v0.9.598)
    g_state.noUpgradeLooks = ReadBool("Weapons", "NoUpgradeLooks", false, path);       // (v0.9.606)

    
    
    
    
    g_state.clothWetConfigUnderwater = false;

    g_state.instanceHighlightSelected = ReadBool("Instances", "HighlightSelected", true, path);
    g_state.blendShapeBufferMB = std::clamp(GetPrivateProfileIntA("Rendering", "BlendShapeBufferMB", 64, path), 10u, 512u);
    g_state.lodDistance = std::clamp(ReadFloat("Rendering", "LodDistance", 1.0f, path), 0.5f, 16.0f);
    g_state.foliageLodDistance = std::clamp(ReadFloat("Rendering", "FoliageLodDistance", 1.0f, path), 0.5f, 16.0f);
    g_state.shadowDistance = std::clamp(ReadFloat("Rendering", "ShadowDistance", 1.0f, path), 0.5f, 16.0f);
    g_state.shadowSharpness = std::clamp(ReadFloat("Rendering", "ShadowSharpness", 1.0f, path), 1.0f, 4.0f);
    {
        const int size = static_cast<int>(GetPrivateProfileIntA("Rendering", "ShadowMapSize", 1, path));
        g_state.shadowMapSize = size >= 8 ? 8 : (size >= 4 ? 4 : (size >= 2 ? 2 : 1));
    }
    g_state.fogDistance = std::clamp(ReadFloat("Rendering", "FogDistance", 1.0f, path), 0.5f, 4.0f);
    g_state.hairSelfShadowDistance = std::clamp(ReadFloat("Rendering", "HairSelfShadowDistance", 10.0f, path), 1.0f, 40.0f);
    g_state.lampShadowSoftness = std::clamp(static_cast<int>(GetPrivateProfileIntA("Rendering", "LampShadowSoftness", 1, path)), 0, 3);
    g_state.contactShadows = std::clamp(static_cast<int>(GetPrivateProfileIntA("Rendering", "ContactShadows", 2, path)), 0, 3);
    g_state.contactShadowLength = std::clamp(ReadFloat("Rendering", "ContactShadowLength", 12.0f, path), 4.0f, 30.0f);
    g_state.rtLampShadows = GetPrivateProfileIntA("Rendering", "RtLampShadows", 0, path) != 0;   // (v0.9.561)
    g_state.rtLampSoftness = std::clamp(ReadFloat("Rendering", "RtLampSoftness", 1.0f, path), 0.0f, 3.0f);   // (v0.9.564)
    g_state.rtSunShadows = GetPrivateProfileIntA("Rendering", "RtSunShadows", 0, path) != 0;   // (v0.9.570)
    g_state.skinScattering = ReadBool("Rendering", "SkinScattering", true, path);   // (v0.9.540)
    ReadString("Hair", "StrandLayout", "", g_state.hairStrandLayout, sizeof(g_state.hairStrandLayout), path);
    g_state.hairSimTempArray = std::clamp(static_cast<int>(GetPrivateProfileIntA("Hair", "SimTempArray", 0, path)), 0, 256);
    ReadString("Hair", "LockedMotionProfile", "", g_state.hairLockedMotionProfile,
               sizeof(g_state.hairLockedMotionProfile), path);   // (v0.9.586)
}

void Save() {
    const char* path = IniPath();
    if (g_state.uiScaleSaved) WriteFloat("UI", "Scale", g_state.uiScale, path);   // (v1.0.0: not before it is chosen)
    WriteInt("UI", "Theme", g_state.uiTheme, path);
    WriteFloat("Camera", "FovMultiplier", g_state.fovMultiplier, path);
    WriteFloat("Player", "DeathReloadDelay", g_state.deathReloadDelay, path);
    WriteFloat("Draw", "InstancesDistance", g_state.drawInstancesDistance, path);
    WriteFloat("Draw", "MarkupDistance", g_state.drawMarkupDistance, path);
    WritePrivateProfileStringA("Draw", "CollisionDistance", nullptr, path);   // (v0.9.597: Draw collision removed)
    {   // (v0.9.607) Draw wireframe's options
        char text[96]{};
        WriteInt("Draw", "WireframeStyle", g_state.wireframeStyle, path);
        sprintf_s(text, "%.3f %.3f %.3f %.3f", g_state.wireframeColor[0], g_state.wireframeColor[1], g_state.wireframeColor[2],
                  g_state.wireframeColor[3]);
        WriteString("Draw", "WireframeColor", text, path);
        sprintf_s(text, "%.3f %.3f %.3f", g_state.wireframeBackground[0], g_state.wireframeBackground[1],
                  g_state.wireframeBackground[2]);
        WriteString("Draw", "WireframeBackground", text, path);
        WriteBool("Draw", "WireframeSeeThrough", g_state.wireframeSeeThrough, path);
        WriteBool("Draw", "WireframeBackFaces", g_state.wireframeBackFaces, path);
    }
    WritePrivateProfileStringA("Endurance", nullptr, nullptr, path);   // (v0.9.607: the Endurance tab removed)
    WriteInt("Settings", "Version", kIniVersion, path);
    WriteBool("Loading", "FasterLoading", g_state.fasterLoading, path);
    // (v0.9.459) the keys FasterLoading replaced, taken out of the ini
    for (const char* key : {"FastStartup", "SkipIntroLogos"})
        WritePrivateProfileStringA("Startup", key, nullptr, path);
    for (const char* key : {"FasterLoadingTimes", "FastLoading", "FastLoadingAggressiveRenderer",
                            "FastLoadingNonBlockingPresent"})
        WritePrivateProfileStringA("Loading", key, nullptr, path);
    WriteBool("General", "DoNotPauseOnFocusLoss", g_state.doNotPauseOnFocusLoss, path);
    WriteBool("General", "AllowCampsitesNearEnemies", g_state.allowCampsitesNearEnemies, path);
    WriteBool("Gameplay", "HideHints", g_state.hideHints, path);   // (v0.9.586)
    WriteBool("Gameplay", "NoDrawOnSwitch", g_state.noDrawOnWeaponSwitch, path);
    WriteBool("Gameplay", "HideLowerGear", g_state.hideLowerGear, path);
    WriteBool("Gameplay", "HideUpperGear", g_state.hideUpperGear, path);
    WriteBool("Gameplay", "HideBodyGear", g_state.hideBodyGear, path);
    WriteBool("Gameplay", "HideQuiverStrap", g_state.hideQuiverStrap, path);
    WriteBool("Gameplay", "WeaponSelectBottomLeft", g_state.weaponSelectBottomLeft, path);   // (v0.9.589)
    WriteInt("Gameplay", "WeaponSelectSize", g_state.weaponSelectSize, path);   // (v0.9.590)
    WriteBool("Outfits", "NoCutsceneOutfitChanges", g_state.noCutsceneOutfitChanges, path);
    WriteBool("General", "ClothLevelCollision", g_state.clothLevelCollision, path);
    WriteBool("General", "FixCutsceneBonePositions", g_state.fixCutsceneBonePositions, path);
    WriteBool("Weapons", "DualWieldPistols", g_state.dualWieldPistols, path);
    WriteBool("Weapons", "SilencerToggle", g_state.silencerToggle, path);
    WriteBool("Weapons", "SilencerNoReload", g_state.silencerNoReload, path);
    WriteBool("Weapons", "NoUpgradeLooks", g_state.noUpgradeLooks, path);
    WriteInt("Rendering", "BlendShapeBufferMB", g_state.blendShapeBufferMB, path);
    WriteFloat("Rendering", "LodDistance", g_state.lodDistance, path);
    WriteFloat("Rendering", "FoliageLodDistance", g_state.foliageLodDistance, path);
    WriteFloat("Rendering", "ShadowDistance", g_state.shadowDistance, path);
    WriteFloat("Rendering", "ShadowSharpness", g_state.shadowSharpness, path);
    WriteInt("Rendering", "ShadowMapSize", g_state.shadowMapSize, path);
    WritePrivateProfileStringA("Rendering", "ShadowResolution", nullptr, path);   // (removed in v0.9.490)
    WriteFloat("Rendering", "FogDistance", g_state.fogDistance, path);
    WriteFloat("Rendering", "HairSelfShadowDistance", g_state.hairSelfShadowDistance, path);
    WriteInt("Rendering", "LampShadowSoftness", g_state.lampShadowSoftness, path);
    WriteInt("Rendering", "ContactShadows", g_state.contactShadows, path);
    WriteFloat("Rendering", "ContactShadowLength", g_state.contactShadowLength, path);
    WriteInt("Rendering", "RtLampShadows", g_state.rtLampShadows ? 1 : 0, path);   // (v0.9.561)
    WriteFloat("Rendering", "RtLampSoftness", g_state.rtLampSoftness, path);          // (v0.9.564)
    WriteInt("Rendering", "RtSunShadows", g_state.rtSunShadows ? 1 : 0, path);         // (v0.9.570)
    WritePrivateProfileStringA("Rendering", "SkinLighting", nullptr, path);   // (0.9.530 only, removed in 0.9.531)
    WritePrivateProfileStringA("Rendering", "HairShadows", nullptr, path);   // (v0.9.607: the switch removed)
    WriteBool("Rendering", "SkinScattering", g_state.skinScattering, path);
    WritePrivateProfileStringA("Rendering", "EarTransmission", nullptr, path);   // (v0.9.541: part of SkinScattering)
    WriteBool("Instances", "HighlightSelected", g_state.instanceHighlightSelected, path);
    WriteString("Hair", "StrandLayout", g_state.hairStrandLayout, path);
    WriteInt("Hair", "SimTempArray", g_state.hairSimTempArray, path);
    WriteString("Hair", "LockedMotionProfile", g_state.hairLockedMotionProfile, path);   // (v0.9.586)

    
    
    WriteBool("Cloth", "WetConfigUnderwater", false, path);
    WriteBool("Cloth", "ForceWetConfig", false, path);
    WriteBool("Cloth", "ZeroGravityUnderwater", false, path);
    WriteBool("Cloth", "PureHairWaterBehavior", false, path);
}
}
