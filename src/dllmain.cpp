#include "cutscene_viewer.h"
#include "cutscenes.h"
#include "steam_save_cache.h"
#include "log.h"
#include "crash_diagnostics.h"
#include "game_build.h"
#include "bone_patch.h"
#include "eight_weight_patch.h"
#include "shadow_blend.h"
#include "blendshape_patch.h"
#include "hair_layout.h"
#include "settings.h"
#include "features.h"
#include "overlay.h"
#include "script_engine.h"
#include "swim_breath.h"
#include "combat_air.h"
#include "acro.h"
#include "grapple.h"
#include "enemy_ai.h"
#include "brawl.h"
#include "cursor_release.h"
#include "version.h"
#include "dtp_edit.h"
#include "suite_link.h"
#include "instance_anim.h"
#include "custom_outfits.h"
#include "glow_color.h"
#include "language_options.h"
#include "silencer.h"
#include "hud_watch.h"
#include "dialogue_guard.h"
#include "rt_probe.h"
#include "dx12_root_fix.h"
#include "dlaa.h"
#include "render_tweaks.h"
#include "larafm.h"
#include "job_threads.h"
#include <Windows.h>
#include <MinHook.h>
#include <cstdint>
#include <cstddef>
#include <cstring>

namespace {
// ROTTRFramework.ini [Startup] SkipLauncher=1: faultrep.dll turns off the launcher window before the game starts
void LogLauncherSkipState() {
    HMODULE faultrep = GetModuleHandleW(L"faultrep.dll");
    using StateFn = unsigned int(WINAPI*)();
    auto stateFn = faultrep ? reinterpret_cast<StateFn>(GetProcAddress(faultrep, "RottrLauncherSkipState")) : nullptr;
    if (!stateFn) return;
    const unsigned int state = stateFn();
    if (state & 1u) Log::Detail("Startup: launcher window skipped ([Startup] SkipLauncher=1)");
    if (state & 2u) Log::Write("Startup: launcher skip wanted but the game code differs (not patched)");
}

DWORD WINAPI InitThread(void* moduleParam) {
    Log::Init();
    Log::Write("ROTTRFramework v" ROTTRFRAMEWORK_VERSION " starting");
    Log::Write(Log::Verbose() ? "Log: every line ([Log] Verbose=1)"
                              : "Log: important lines only ([Log] Verbose=1 in ROTTRFramework.ini writes every line)");

    if (!GameBuild::IsSupportedRetail()) {
        Log::Write("Unsupported ROTTR.exe build; ROTTRFramework will remain inactive");
        return 0;
    }

    
    
    
    Settings::Load();
    // (v0.9.518) before the game makes its render device and, with it, the hair's lighting pass
    ShadowBlend::SetHairSelfShadowDistance(Settings::Get().hairSelfShadowDistance);
    ShadowBlend::SetLampShadowSoftness(Settings::Get().lampShadowSoftness);   // (v0.9.525)
    ShadowBlend::SetContactShadows(Settings::Get().contactShadows);            // (v0.9.526)
    ShadowBlend::SetContactShadowLength(Settings::Get().contactShadowLength);
    // (v0.9.561) ray-traced lamp shadows; (v0.9.562) the lights' pass reads them once the probe has checked its table
    RtProbe::SetLampShadows(Settings::Get().rtLampShadows);
    RtProbe::SetLampSoftness(Settings::Get().rtLampSoftness);   // (v0.9.564)
    RtProbe::SetSunShadows(Settings::Get().rtSunShadows);       // (v0.9.570)
    // (v0.9.440) PureHair's strand layout ([Hair] StrandLayout): before the game makes its render device and, with it,
    // the hair's shaders
    HairLayout::ApplyAtStartup(Settings::Get().hairStrandLayout, Settings::Get().hairSimTempArray);
    Features::SyncFromSettings();
    
    
    
    LogLauncherSkipState();

    if (MH_Initialize() != MH_OK) {
        Log::Write("MinHook initialization failed; overlay/features unavailable");
        return 0;
    }
    // (v0.9.517) the game's D3D12 device reached as it is made, before the custom outfits' archive is built: its
    // pipelines are made from the start (the sun's shadow pass among them, shadow_blend.h)
    if (!EightWeightPatch::InstallDeviceHook())
        Log::Write("8W: D3D12CreateDevice not hooked at start-up (tried again with the skinning patch)");

    // Custom outfits with new models: their archive is built now, before the game looks for its archives.
    CustomOutfits::StartEarly();

    
    
    
    
    if (!EightWeightPatch::Install())
        Log::Write("Warning: integrated 8-weight skinning patch failed to install");
    {   // (v0.9.607) Draw wireframe's saved options
        const auto& s = Settings::Get();
        EightWeightPatch::WireframeOptions wire;
        wire.style = s.wireframeStyle;
        std::memcpy(wire.lineColor, s.wireframeColor, sizeof(wire.lineColor));
        std::memcpy(wire.fillColor, s.wireframeBackground, sizeof(wire.fillColor));
        wire.seeThrough = s.wireframeSeeThrough;
        wire.backFaces = s.wireframeBackFaces;
        EightWeightPatch::SetWireframeOptions(wire);
    }

    // Must run before the renderer's LoadSharedResources creates the blendshape output buffer.
    if (!BlendShapePatch::Install())
        Log::Write("Warning: blendshape buffer patch not fully installed");

    // (v0.9.457) DX12 tessellation's black textures: before the renderer draws (its hooks enabled with the startup group)
    Dx12RootFix::SetEnabled(true);   // (v0.9.486: always on, no setting)
    const bool dx12RootFix = Dx12RootFix::Install();
    // (v0.9.484; v0.9.485: an entry of Options > Display > DLSS) DLAA: before the swap chain is made (its first set-up)
    const bool dlaaHook = Dlaa::Install();
    // (v0.9.487) LOD / shadow / fog: before the shadow maps are made
    RenderTweaks::SetLodDistance(Settings::Get().lodDistance);
    RenderTweaks::SetFoliageLodDistance(Settings::Get().foliageLodDistance);   // (v0.9.519)
    RenderTweaks::SetSkinScattering(Settings::Get().skinScattering);           // (v0.9.540; v0.9.541: the ears too)
    RenderTweaks::SetShadowDistance(Settings::Get().shadowDistance);
    RenderTweaks::SetShadowSharpness(Settings::Get().shadowSharpness);
    RenderTweaks::SetShadowMapSize(Settings::Get().shadowMapSize);   // (v0.9.523)
    RenderTweaks::SetFogDistance(Settings::Get().fogDistance);
    RenderTweaks::SetHairSelfShadowOffset(0.0f);   // (v0.9.518: the hair's shadow pass as the game makes it)
    const bool renderTweaks = RenderTweaks::Install();
    const bool startupHooks = Features::CreateStartupHooks();
    if (startupHooks || dx12RootFix || dlaaHook || renderTweaks) {
        MH_QueueEnableHook(MH_ALL_HOOKS);
        if (MH_ApplyQueued() != MH_OK)
            Log::Write("Failed to enable early startup hook");
    }

    
    
    Log::Detail("Init: entering post-startup 1s stabilization delay");
    Sleep(1000);
    Log::Detail("Init: stabilization delay complete; installing 8191 patch");

    
    
    if (!BonePatch::Install())
        Log::Write("Warning: integrated 8191-bone patch failed to install");

    Log::Detail("Init: creating feature hooks");
    const bool featureHooks = Features::CreateHooks();
    Log::Detail("Init: feature hook creation returned");
    const bool swimHooks = SwimBreath::CreateHooks();
    const bool combatAirHooks = CombatAir::CreateHooks();
    const bool instanceAnimHooks = InstanceAnim::CreateHooks();  // after CombatAir: shares its AnimBuildTransforms hook
    const bool customOutfitHooks = CustomOutfits::Install();   // ROTTRFramework/outfits/*
    const bool silencerHooks = Silencer::Install();             // (v0.9.456) the weapon's key: the silencer off and on
    Silencer::SetEnabled(Settings::Get().silencerToggle);
    Silencer::SetNoReload(Settings::Get().silencerNoReload);   // (v0.9.598)
    Silencer::SetPlainLooks(Settings::Get().noUpgradeLooks);   // (v0.9.606)
    const bool hudWatchHooks = HudWatch::Install();             // (v0.9.541) logging when the HUD stops answering
    const bool dialogueGuardHook = DialogueGuard::Install();    // (v0.9.542) a respawned speaker's stale dialogue
    const bool rtProbeHook = RtProbe::Install();                // (v0.9.543) the ray tracing probe's engine draw hook
    const bool steamSaveHook = SteamSaveCache::CreateHook();
    Cutscenes::Initialize();
    const bool cutsceneHooks = Cutscenes::CreateHooks();      // the levels' cutscene nodes
    const bool viewerHooks = CutsceneViewer::CreateHooks();   // the main menu's EXTRA
    Acro::Initialize();
    Grapple::Initialize();
    EnemyAI::Initialize();
    Brawl::Initialize();
    // (v1.0.0) the Foundation Engine editor's link removed: the game data edits (the Suite's, the voice and weapon
    // edits') and the cursor's release (Save As, another window in front) started here, as EditorLink did
    DtpEdit::Initialize();
    CursorRelease::Initialize();
    SuiteLink::Initialize();   // (after DtpEdit)
    const bool glowHook = GlowColor::Initialize();   // (v0.9.472, contributed) Lara's glowstick in a colour of your own
    const bool laraFmHook = LaraFM::Install();   // (v0.9.491) your own music on a radio in the game
    LanguageOptions::Install();   // (v0.9.473) Options > Audio's languages in game too
    const bool overlayHooks = Overlay::CreateHooks();
    if (!featureHooks) Log::Write("Feature hook group failed; continuing with remaining modules");
    if (!overlayHooks) Log::Write("Overlay hook group failed; ROTTRFramework menu unavailable");

    if (featureHooks || swimHooks || combatAirHooks || instanceAnimHooks || customOutfitHooks || silencerHooks || hudWatchHooks || dialogueGuardHook || rtProbeHook || steamSaveHook ||
        cutsceneHooks || viewerHooks || overlayHooks || glowHook || laraFmHook) {
        MH_QueueEnableHook(MH_ALL_HOOKS);
        if (MH_ApplyQueued() != MH_OK) {
            Log::Write("Failed to enable MinHook hooks");
            return 0;
        }
    }

    
    
    
    ScriptEngine::Initialize();

    // Dual wield pistols start on unless turned off in the menu ([Weapons] DualWieldPistols=0).
    if (Settings::Get().dualWieldPistols) {
        if (Features::DualWieldExperimentalAvailable()) {
            Features::SetDualWieldExperimentalEnabled(true);
            Log::Detail("Init: dual wield pistols on (ROTTRFramework.ini [Weapons] DualWieldPistols=1)");
        } else {
            Log::Write("Init: dual wield pistols wanted but its hooks are unavailable");
        }
    }

    
    
    CrashDiagnostics::Install(reinterpret_cast<HMODULE>(moduleParam));
    CrashDiagnostics::SetBreadcrumb("ROTTRFramework initialization finished");
    Features::MarkFrameworkInitFinished();
    Log::Write("ROTTRFramework v" ROTTRFRAMEWORK_VERSION " initialization finished");
    return 0;
}
}

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(module);
        JobThreads::PatchEarly();   // (v0.9.606) before the game makes its job system
        HANDLE thread = CreateThread(nullptr, 0, InitThread, module, 0, nullptr);
        if (thread) CloseHandle(thread);
    }
    return TRUE;
}
