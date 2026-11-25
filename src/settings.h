#pragma once
namespace Settings {
struct State {
    
    float uiScale = 1.0f;
    // (v1.0.0) [UI] Scale was in the ini (or chosen since): false on the first run, until overlay.cpp has chosen a
    // scale from the game's resolution (written to the ini only once it is true)
    bool uiScaleSaved = false;
    
    int uiTheme = 0;
    float fovMultiplier = 1.0f;
    
    float deathReloadDelay = 0.0f;
    
    float drawInstancesDistance = 1500.0f;
    
    float drawMarkupDistance = 1500.0f;   // (v0.9.597: [Draw] MarkupDistance; was Draw collision's CollisionDistance)
    // (v0.9.607) Draw wireframe's options ([Draw] WireframeStyle 0 / 1, WireframeColor "r g b a", WireframeBackground
    // "r g b", WireframeSeeThrough, WireframeBackFaces); the switch itself is not saved
    int wireframeStyle = 0;
    float wireframeColor[4] = {0.35f, 0.85f, 1.0f, 1.0f};
    float wireframeBackground[3] = {0.08f, 0.08f, 0.09f};
    bool wireframeSeeThrough = false;
    bool wireframeBackFaces = false;
    // (v0.9.459) [Loading] FasterLoading, default on: the intro logos skipped and the startup sped up (the old Fast
    // startup; faultrep.dll reads the same key) and the level loads sped up (the old Faster loading times), one option.
    bool fasterLoading = true;

    // [General] DoNotPauseOnFocusLoss: the game keeps running in the background (default on since v0.9.459).
    bool doNotPauseOnFocusLoss = true;
    
    bool allowCampsitesNearEnemies = false;
    // (v0.9.586) Enhancements > Gameplay ([Gameplay]): no tutorial hints and no "basecamp / tomb discovered", "tomb
    // nearby" notifications (HideHints); switching weapons only chooses one, Lara does not draw it (NoDrawOnSwitch);
    // her lower / upper gear as the Modding Suite hides it per outfit, for every outfit (HideLowerGear, HideUpperGear);
    // draw groups 46-48 (bandolier, rucksack, satchel: HideBodyGear) and 12 (the quiver strap: HideQuiverStrap).
    bool hideHints = false;
    bool noDrawOnWeaponSwitch = false;
    bool hideLowerGear = false;
    bool hideUpperGear = false;
    bool hideBodyGear = false;
    bool hideQuiverStrap = false;
    bool weaponSelectBottomLeft = false;   // (v0.9.589) [Gameplay] WeaponSelectBottomLeft: the HUD's weapon select there
    int weaponSelectSize = 65;             // (v0.9.590) [Gameplay] WeaponSelectSize: its size there, % of the game's

    // [Outfits] NoCutsceneOutfitChanges: the story's scripts do not change Lara's outfit (AGPlayerRespawnNode).
    bool noCutsceneOutfitChanges = false;
    
    bool clothLevelCollision = false;
    
    bool fixCutsceneBonePositions = false;

    // Dual wield pistols (Weapons tab), turned on at startup ([Weapons] DualWieldPistols, default on).
    bool dualWieldPistols = true;
    // (v0.9.456) The weapon's key pressed again takes the silencer off and puts it back (Weapons tab;
    // [Weapons] SilencerToggle, default on; silencer.h).
    bool silencerToggle = true;
    // (v0.9.598) [Weapons] SilencerNoReload, default on: a silencer toggle without Lara's reload (silencer.h).
    bool silencerNoReload = true;
    // (v0.9.606) [Weapons] NoUpgradeLooks, default off: the weapons drawn without their upgrades' looks (silencer.h).
    bool noUpgradeLooks = false;

    
    
    bool clothWetConfigUnderwater = false;

    // [Rendering] BlendShapeBufferMB: size of the per-frame blendshape (facial morph) output buffer.
    // Vanilla is 10 MB (~437k morphed vertices per frame); read once at startup, 10..512.
    int blendShapeBufferMB = 64;
    // (v0.9.487) render_tweaks.h: the LOD distance, shadow distance / sharpness (v0.9.490) and fog distance
    // multipliers (1 = the game's)
    float lodDistance = 1.0f;
    float foliageLodDistance = 1.0f;   // (v0.9.519)
    float shadowDistance = 1.0f;
    float shadowSharpness = 1.0f;
    int shadowMapSize = 1;   // (v0.9.523: each light's shadow map x1 / x2 / x4 while the atlas has room; v0.9.524: x8)
    float fogDistance = 1.0f;
    int lampShadowSoftness = 1;   // (v0.9.525: 0 the game's lamp shadows; 1 / 2 / 3 softer: shadow_blend.h)
    int contactShadows = 2;            // (v0.9.526: 0 off; 1 / 2 / 3 light / medium / strong: shadow_blend.h)
    float contactShadowLength = 12.0f;   // (v0.9.526: cm, 4 .. 30)
    bool rtLampShadows = false;          // (v0.9.561) ray-traced lamp shadows (the ray tracing probe's mask)
    float rtLampSoftness = 1.0f;         // (v0.9.564) their softness: 0 sharp .. 3 (v0.9.570: the sun's too)
    bool rtSunShadows = false;           // (v0.9.570) ray-traced sun shadows (the probe's sun mask)
    float hairSelfShadowDistance = 10.0f;   // (v0.9.518, cm: the hair does not shadow itself within it; 1: the game's)
    bool skinScattering = true;    // (v0.9.540: the skin's subsurface scattering in play too: render_tweaks.h;
                                   // v0.9.541: the ears' light transmission with it)

    // [Instances] HighlightSelected: the selected Instance keeps a yellow material tint.
    bool instanceHighlightSelected = true;

    // [Hair] StrandLayout (v0.9.440): PureHair's strand layout put into the game's memory at startup (hair_layout.cpp):
    // empty / Off the game's own, Stock, PH64, PH128 or PH256. SimTempArray: PH128 / PH256's simulation arrays (0: the
    // layout's points; 16..256, for a GPU that refuses or crawls at 256). Read once at startup.
    char hairStrandLayout[16] = "";
    int hairSimTempArray = 0;
    // (v0.9.586) [Hair] LockedMotionProfile: the motion profile (by name) Lara's hair always uses, whatever the game
    // switches to (empty: the game chooses).
    char hairLockedMotionProfile[96] = "";
};
State& Get();
void Load();
void Save();
}
