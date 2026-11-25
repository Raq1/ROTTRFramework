#include "stall_sampler.h"
#include "overlay.h"
#include "cutscene_viewer.h"
#include "ui.h"
#include "script_engine.h"
#include "features.h"
#include "settings.h"
#include "log.h"
#include "dx12_overlay.h"
#include "eight_weight_patch.h"
#include "rt_probe.h"
#include "larafm.h"
#include <Windows.h>
#include <d3d11.h>
#include <d3d12.h>
#include <d3dcompiler.h>
#include <dxgi.h>
#include <MinHook.h>
#include <imgui.h>
#include <atomic>
#include <iterator>
#include <cmath>
#include <cstdio>
#include <vector>
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <climits>
#include <chrono>
#include <backends/imgui_impl_win32.h>
#include <backends/imgui_impl_dx11.h>

extern LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

namespace {
using PresentFn = HRESULT(__stdcall*)(IDXGISwapChain*, UINT, UINT);
using ResizeBuffersFn = HRESULT(__stdcall*)(IDXGISwapChain*, UINT, UINT, UINT, DXGI_FORMAT, UINT);

PresentFn g_originalPresent = nullptr;
ResizeBuffersFn g_originalResizeBuffers = nullptr;
PresentFn g_originalPresentAlt = nullptr;
ResizeBuffersFn g_originalResizeBuffersAlt = nullptr;

enum class RendererApi { Unknown, DirectX11, DirectX12 };
RendererApi g_rendererApi = RendererApi::Unknown;
ID3D11Device* g_device = nullptr;
ID3D11DeviceContext* g_context = nullptr;
ID3D11RenderTargetView* g_rtv = nullptr;
HWND g_hwnd = nullptr;
WNDPROC g_oldWndProc = nullptr;
bool g_imguiReady = false;
bool g_imguiContextReady = false;
bool g_win32Ready = false;
bool g_menuOpen = false;
std::atomic<std::uint32_t> g_emptyOverlayFrames{0};   // (v0.9.602) presents whose overlay frame drew nothing: not submitted
bool g_menuPassThrough = false;



bool g_playerInputLocked = false;
bool g_freeCameraAutoLockedPlayerInput = false;
bool g_focusTransitionPassThrough = false;
bool g_hooksCreated = false;
bool g_loggedDx12WaitingQueue = false;
HCURSOR g_arrowCursor = nullptr;
bool g_rtvIsSrgb = false;
DXGI_FORMAT g_rtvFormat = DXGI_FORMAT_UNKNOWN;
bool g_styleApplied = false;
bool g_styleAppliedForSrgb = false;
int g_appliedUiTheme = -1;
float g_appliedUiScale = 1.0f;







bool g_legalMaskWasActive = false;
bool g_legalMaskSafetyReleased = false;
bool g_legalMaskFinished = false;
bool g_legalFirstUnmaskedPresentPending = false;
bool g_legalFirstUnmaskedPresentLogged = false;
ULONGLONG g_legalMaskStartedAt = 0;
constexpr ULONGLONG kLegalMaskSafetyMs = 15000ull;

bool IsSrgbFormat(DXGI_FORMAT format) {
    switch (format) {
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
    case DXGI_FORMAT_B8G8R8X8_UNORM_SRGB:
        return true;
    default:
        return false;
    }
}

float SrgbToLinear(float c) {
    if (c <= 0.04045f)
        return c / 12.92f;
    return powf((c + 0.055f) / 1.055f, 2.4f);
}

void DestroyRtv() {
    if (g_rtv) { g_rtv->Release(); g_rtv = nullptr; }
    g_rtvFormat = DXGI_FORMAT_UNKNOWN;
    g_rtvIsSrgb = false;
}
void CreateRtv(IDXGISwapChain* swap) {
    DestroyRtv();
    ID3D11Texture2D* backBuffer = nullptr;
    if (SUCCEEDED(swap->GetBuffer(0, IID_PPV_ARGS(&backBuffer)))) {
        if (SUCCEEDED(g_device->CreateRenderTargetView(backBuffer, nullptr, &g_rtv)) && g_rtv) {
            D3D11_RENDER_TARGET_VIEW_DESC rtvDesc{};
            g_rtv->GetDesc(&rtvDesc);
            g_rtvFormat = rtvDesc.Format;
            g_rtvIsSrgb = IsSrgbFormat(g_rtvFormat);
        }
        backBuffer->Release();
    }
}

bool IsGameInputMessage(UINT msg) {
    switch (msg) {
    case WM_INPUT:
    case WM_MOUSEMOVE:
    case WM_LBUTTONDOWN:
    case WM_LBUTTONUP:
    case WM_LBUTTONDBLCLK:
    case WM_RBUTTONDOWN:
    case WM_RBUTTONUP:
    case WM_RBUTTONDBLCLK:
    case WM_MBUTTONDOWN:
    case WM_MBUTTONUP:
    case WM_MBUTTONDBLCLK:
    case WM_XBUTTONDOWN:
    case WM_XBUTTONUP:
    case WM_XBUTTONDBLCLK:
    case WM_MOUSEWHEEL:
    case WM_MOUSEHWHEEL:
    case WM_KEYDOWN:
    case WM_KEYUP:
    case WM_SYSKEYDOWN:
    case WM_SYSKEYUP:
    case WM_CHAR:
        return true;
    default:
        return false;
    }
}

// The overlay takes mouse and keyboard: the framework menu (not passed through) or the Cutscene Viewer.
bool OverlayOwnsInput() { return (g_menuOpen && !g_menuPassThrough) || CutsceneViewer::WantsInput(); }

void ApplyMenuFocusState() {
    const bool trainerOwnsInput = OverlayOwnsInput();
    Features::SetTrainerMenuOpen(trainerOwnsInput);
    if (!g_imguiContextReady || !g_win32Ready) return;

    ImGuiIO& io = ImGui::GetIO();
    
    
    
    io.MouseDrawCursor = trainerOwnsInput;
    ImGui::GetStyle().MouseCursorScale = 1.0f;

    if (trainerOwnsInput) {
        
        
        ClipCursor(nullptr);
        if (!g_arrowCursor)
            g_arrowCursor = LoadCursorW(nullptr, IDC_ARROW);
        if (g_arrowCursor)
            SetCursor(g_arrowCursor);
    } else if (g_menuOpen && g_menuPassThrough) {
        
        
        SetCursor(nullptr);
    }
}

LRESULT CALLBACK HookWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    
    
    
    if (msg == WM_KILLFOCUS ||
        (msg == WM_ACTIVATEAPP && wParam == FALSE) ||
        (msg == WM_ACTIVATE && LOWORD(wParam) == WA_INACTIVE)) {
        ClipCursor(nullptr);
        Features::NotifyFreeCameraFocusLost();
    }
    // (v0.9.491) LaraFM's arrow keys in play (they go on to the game as well: it binds none of them by default)
    LaraFM::HandleKey(msg, static_cast<unsigned long long>(wParam), static_cast<long long>(lParam));

    
    
    
    
    const bool altF4 =
        (msg == WM_SYSKEYDOWN || msg == WM_KEYDOWN) &&
        wParam == VK_F4 &&
        (((static_cast<unsigned long long>(lParam) & (1ull << 29)) != 0) ||
         ((GetAsyncKeyState(VK_MENU) & 0x8000) != 0));
    if (altF4) {
        Log::Write("Input: ALT+F4 received; exiting ROTTR immediately (quit confirmation bypassed)");
        ExitProcess(0);
    }
    
    if ((msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN) && wParam == VK_F6)
        return 0;
    if ((msg == WM_KEYUP || msg == WM_SYSKEYUP) && wParam == VK_F6) {
        if (Features::HudControlsAvailable()) {
            const bool next = !Features::HudHidden();
            Features::SetHudHidden(next);
            Log::Detailf("Input: F6 game UI %s", next ? "hidden" : "shown");
        } else {
            Log::Write("Input: F6 ignored; game UI control unavailable");
        }
        return 0;
    }

    
    
    
    if ((msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN) && wParam == VK_F7)
        return 0;
    if ((msg == WM_KEYUP || msg == WM_SYSKEYUP) && wParam == VK_F7) {
        if (Features::DepthOfFieldAvailable()) {
            const bool next = !Features::DepthOfFieldEnabled();
            Features::SetDepthOfFieldEnabled(next);
            Log::Detailf("Input: F7 depth of field %s", next ? "enabled" : "disabled");
        } else {
            Log::Write("Input: F7 ignored; Rise DOF renderer hook unavailable");
        }
        return 0;
    }

    
    
    if ((msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN) && wParam == VK_F8 &&
        (static_cast<unsigned long long>(lParam) & (1ull << 30)) == 0) {
        const bool shiftHeld = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
        if (g_menuOpen && shiftHeld) {
            const bool wasPassThrough = g_menuPassThrough;
            g_menuPassThrough = !g_menuPassThrough;
            
            
            
            
            g_focusTransitionPassThrough = wasPassThrough && !g_menuPassThrough;
        } else {
            g_menuOpen = !g_menuOpen;
            
            g_menuPassThrough = false;
            g_focusTransitionPassThrough = false;
        }
        ApplyMenuFocusState();
        return 0;
    }
    
    
    if ((msg == WM_KEYUP || msg == WM_SYSKEYUP) && wParam == VK_F8)
        return 0;

    
    
    if ((msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN) && wParam == VK_F2)
        return 0;
    if ((msg == WM_KEYUP || msg == WM_SYSKEYUP) && wParam == VK_F2) {
        Features::SetFlyMode(!Features::FlyModeEnabled());
        return 0;
    }

    
    
    if ((msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN) && wParam == VK_F3)
        return 0;
    if ((msg == WM_KEYUP || msg == WM_SYSKEYUP) && wParam == VK_F3) {
        if (Features::PauseAvailable()) {
            const bool next = !Features::GamePaused();
            if (Features::SetGamePaused(next))
                Log::Detailf("Input: F3 %s game pause", next ? "enabled" : "disabled");
        } else {
            Log::Write("Input: F3 ignored; pause control unavailable");
        }
        return 0;
    }

    if ((msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN) && wParam == VK_F4)
        return 0;
    if ((msg == WM_KEYUP || msg == WM_SYSKEYUP) && wParam == VK_F4) {
        Overlay::SetFreeCameraEnabled(!Features::FreeCameraEnabled());
        return 0;
    }

    
    
    
    if ((msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN) && wParam == VK_F5)
        return 0;
    if ((msg == WM_KEYUP || msg == WM_SYSKEYUP) && wParam == VK_F5) {
        g_playerInputLocked = !g_playerInputLocked;
        g_freeCameraAutoLockedPlayerInput = false;
        Log::Detailf("Input: F5 player input %s", g_playerInputLocked ? "locked" : "unlocked");
        return 0;
    }

    
    if ((msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN) && wParam == VK_F9)
        return 0;
    if ((msg == WM_KEYUP || msg == WM_SYSKEYUP) && wParam == VK_F9) {
        if (Features::CutscenePlaying())   // (v0.9.440: the switch respawns Lara, which ends the cutscene)
            Log::Write("Input: F9 ignored: a cutscene plays");
        else if (Features::RequestNextOutfit())
            Log::Detail("Input: F9 queued next outfit");
        else
            Log::Write("Input: F9 outfit cycle unavailable/busy");
        return 0;
    }

    // Esc while a cutscene plays from the Cutscene Viewer: back to the menu
    if (CutsceneViewer::HandleKey(msg, wParam))
        return 0;

    if ((msg == WM_KEYUP || msg == WM_SYSKEYUP) && wParam == VK_F11) {
        Features::RequestKillPlayer();   // (v0.9.440: refused while a cutscene plays)
        return 0;
    }

    const bool trainerOwnsInput = OverlayOwnsInput();
    if (trainerOwnsInput && g_imguiContextReady && g_win32Ready) {
        ImGui_ImplWin32_WndProcHandler(hwnd, msg, wParam, lParam);

        if (msg == WM_SETCURSOR) {
            if (!g_arrowCursor)
                g_arrowCursor = LoadCursorW(nullptr, IDC_ARROW);
            if (g_arrowCursor)
                SetCursor(g_arrowCursor);
            return TRUE;
        }
    }

    
    
    
    const bool transitionPass = trainerOwnsInput && g_focusTransitionPassThrough;
    // (the framework menu's input: all of it; the Cutscene Viewer's: as it says, the Model Viewer's button bar the game's)
    const bool menuOwnsInput = g_menuOpen && !g_menuPassThrough;
    if (((menuOwnsInput && !transitionPass) || g_playerInputLocked ||
         CutsceneViewer::BlocksGameMessage(hwnd, msg, wParam, lParam)) &&
        IsGameInputMessage(msg))
        return 0;

    const LRESULT result = CallWindowProcW(g_oldWndProc, hwnd, msg, wParam, lParam);
    if (transitionPass && (msg == WM_KEYUP || msg == WM_SYSKEYUP) && wParam == VK_SHIFT)
        g_focusTransitionPassThrough = false;
    return result;
}

bool ShouldMaskStartupLegalPresentation() {
    
    
    
    
    if (g_legalMaskFinished)
        return false;

    const bool requestedActive = Features::StartupLegalVisualSuppressionActive();
    if (!requestedActive) {
        if (g_legalMaskWasActive) {
            Log::Detail("Overlay: startup Legal presentation mask released by native state exit");
            g_legalMaskFinished = true;
            g_legalFirstUnmaskedPresentPending = true;
        }
        g_legalMaskWasActive = false;
        g_legalMaskStartedAt = 0;
        return false;
    }

    const ULONGLONG now = GetTickCount64();
    if (!g_legalMaskWasActive) {
        g_legalMaskWasActive = true;
        g_legalMaskStartedAt = now;
        g_legalMaskSafetyReleased = false;
        Log::Detail("Overlay: hiding startup Legal/logo presentation while native readiness continues");
        const std::uint64_t firstProcessMs = Features::StartupTimingMilestoneMs(0u);
        const std::uint64_t deltaMs = (firstProcessMs && now >= firstProcessMs) ? (static_cast<std::uint64_t>(now) - firstProcessMs) : 0ull;
        Log::Detailf("StartupTiming: +%llu ms | framework Legal black mask first active",
                    static_cast<unsigned long long>(deltaMs));
    }

    if (g_legalMaskStartedAt && now - g_legalMaskStartedAt > kLegalMaskSafetyMs) {
        if (!g_legalMaskSafetyReleased) {
            g_legalMaskSafetyReleased = true;
            Log::Write("Overlay: startup Legal presentation mask safety timeout reached; rendering restored");
        }
        g_legalMaskFinished = true;
        g_legalFirstUnmaskedPresentPending = true;
        return false;
    }
    return true;
}

void ApplyLauMenuHookStyle() {
    
    
    const int theme = std::clamp(Settings::Get().uiTheme, 0, 2);
    if (g_styleApplied && g_styleAppliedForSrgb == g_rtvIsSrgb && g_appliedUiTheme == theme)
        return;

    switch (theme) {
        case 1: ImGui::StyleColorsLight(); break;
        case 2: ImGui::StyleColorsClassic(); break;
        default: ImGui::StyleColorsDark(); break;
    }

    
    
    
    
    
    if (g_rtvIsSrgb) {
        ImGuiStyle& style = ImGui::GetStyle();
        for (int i = 0; i < ImGuiCol_COUNT; ++i) {
            ImVec4& c = style.Colors[i];
            c.x = SrgbToLinear(c.x);
            c.y = SrgbToLinear(c.y);
            c.z = SrgbToLinear(c.z);
        }
    }
    g_styleApplied = true;
    g_styleAppliedForSrgb = g_rtvIsSrgb;
    g_appliedUiTheme = theme;
}

void ApplyUiScaleIfNeeded() {
    if (!g_imguiContextReady) return;

    const float requested = std::clamp(Settings::Get().uiScale, 1.0f, 2.5f);
    if (std::fabs(requested - g_appliedUiScale) <= 0.0001f) return;

    
    
    
    ImGuiStyle& style = ImGui::GetStyle();
    style.ScaleAllSizes(requested / g_appliedUiScale);
    
    
    
    style.MouseCursorScale = 1.0f;
    ImGui::GetIO().FontGlobalScale = requested;
    g_appliedUiScale = requested;
}

bool EnsureImGuiPlatform(HWND hwnd) {
    if (!hwnd) return false;

    if (!g_imguiContextReady) {
        IMGUI_CHECKVERSION();
        ImGui::CreateContext();
        ImGuiIO& io = ImGui::GetIO();
        io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
        io.MouseDrawCursor = g_menuOpen && !g_menuPassThrough;
        ImGui::GetStyle().MouseCursorScale = 1.0f;
        if (io.Fonts->Fonts.empty()) io.Fonts->AddFontDefault();   // (v1.0.0: the scene scripts' message font removed)
        CutsceneViewer::AddFonts();
        g_imguiContextReady = true;
    }

    if (!g_win32Ready) {
        g_hwnd = hwnd;
        if (!ImGui_ImplWin32_Init(g_hwnd))
            return false;
        g_oldWndProc = reinterpret_cast<WNDPROC>(SetWindowLongPtrW(
            g_hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&HookWndProc)));
        g_win32Ready = true;
    }
    return true;
}

RendererApi DetectRenderer(IDXGISwapChain* swap) {
    if (!swap) return RendererApi::Unknown;

    ID3D11Device* d11 = nullptr;
    if (SUCCEEDED(swap->GetDevice(IID_PPV_ARGS(&d11))) && d11) {
        d11->Release();
        return RendererApi::DirectX11;
    }

    ID3D12Device* d12 = nullptr;
    if (SUCCEEDED(swap->GetDevice(IID_PPV_ARGS(&d12))) && d12) {
        
        
        
        
        EightWeightPatch::AttachDx12Device(d12);
        d12->Release();
        return RendererApi::DirectX12;
    }
    return RendererApi::Unknown;
}

bool InitImGuiDx11(IDXGISwapChain* swap) {
    if (FAILED(swap->GetDevice(__uuidof(ID3D11Device), reinterpret_cast<void**>(&g_device))))
        return false;
    g_device->GetImmediateContext(&g_context);

    DXGI_SWAP_CHAIN_DESC desc{};
    if (FAILED(swap->GetDesc(&desc))) return false;
    if (!EnsureImGuiPlatform(desc.OutputWindow)) return false;
    CreateRtv(swap);

    if (!ImGui_ImplDX11_Init(g_device, g_context)) return false;
    g_imguiReady = true;
    ApplyMenuFocusState();
    char formatLine[160]{};
    sprintf_s(formatLine, "Overlay: ImGui RTV format=%u, sRGB=%s", static_cast<unsigned>(g_rtvFormat), g_rtvIsSrgb ? "yes (gamma-compensated)" : "no");
    Log::Detail(formatLine);
    Log::Detail("Overlay: ImGui DX11 initialized on game swap chain (F2 fly; F3 pause; F4 freecam; F5 player input; F6 UI; F7 DOF; F8 menu; F9 next outfit; Shift+F8 visible pass-through mode)");
    return true;
}

bool InitImGuiDx12(IDXGISwapChain* swap) {
    DXGI_SWAP_CHAIN_DESC desc{};
    if (FAILED(swap->GetDesc(&desc))) return false;

    
    
    
    
    Dx12Overlay::PrimeQueueCaptureForSwapDevice(swap);
    if (!Dx12Overlay::QueueCaptured()) {
        if (!g_loggedDx12WaitingQueue) {
            g_loggedDx12WaitingQueue = true;
            Log::Detail("Overlay: DX12 device acquired; waiting for ROTTR's DIRECT command queue submission");
        }
        return false;
    }
    g_loggedDx12WaitingQueue = false;
    if (!EnsureImGuiPlatform(desc.OutputWindow))
        return false;
    if (!Dx12Overlay::TryInitialize(swap))
        return false;

    g_rtvFormat = Dx12Overlay::RenderTargetFormat();
    g_rtvIsSrgb = Dx12Overlay::RenderTargetIsSrgb();
    g_imguiReady = true;
    ApplyMenuFocusState();
    char formatLine[160]{};
    sprintf_s(formatLine, "Overlay: ImGui RTV format=%u, sRGB=%s", static_cast<unsigned>(g_rtvFormat), g_rtvIsSrgb ? "yes (gamma-compensated)" : "no");
    Log::Detail(formatLine);
    Log::Detail("Overlay: ImGui DX12 initialized on game swap chain (F2 fly; F3 pause; F4 freecam; F5 player input; F6 UI; F7 DOF; F8 menu; F9 next outfit; Shift+F8 visible pass-through mode)");
    return true;
}

bool InitImGui(IDXGISwapChain* swap) {
    if (g_imguiReady) return true;
    if (g_rendererApi == RendererApi::Unknown) {
        g_rendererApi = DetectRenderer(swap);
        if (g_rendererApi == RendererApi::DirectX11) {
            Log::Detail("Overlay: game renderer detected: DirectX 11");
            
            
            Dx12Overlay::RetireQueueCaptureHooks();
        } else if (g_rendererApi == RendererApi::DirectX12) {
            Log::Detail("Overlay: game renderer detected: DirectX 12");
        } else {
            return false;
        }
    }

    if (g_rendererApi == RendererApi::DirectX11)
        return InitImGuiDx11(swap);
    if (g_rendererApi == RendererApi::DirectX12)
        return InitImGuiDx12(swap);
    return false;
}

// (v1.0.0) The first run (no [UI] Scale in the ini): the menu's scale from the game's picture, once its height has held
// for 120 presents (about 2 s: the window settles at its size), 1080 lines = 100%, to the nearest of Settings > UI
// scale's sizes; saved then, so that from the next start on the ini's value (the user's choice) is used.
void ChooseFirstRunUiScale(IDXGISwapChain* swap) {
    if (Settings::Get().uiScaleSaved || !swap) return;
    static UINT s_height = 0, s_width = 0;
    static int s_steady = 0;
    DXGI_SWAP_CHAIN_DESC desc{};
    if (FAILED(swap->GetDesc(&desc)) || !desc.BufferDesc.Height || !desc.BufferDesc.Width) return;
    if (desc.BufferDesc.Height != s_height || desc.BufferDesc.Width != s_width) {
        s_height = desc.BufferDesc.Height;
        s_width = desc.BufferDesc.Width;
        s_steady = 0;
        return;
    }
    if (++s_steady < 120) return;
    static constexpr float kScales[] = {1.0f, 1.25f, 1.5f, 1.75f, 2.0f, 2.5f};
    const float wanted = static_cast<float>(s_height) / 1080.0f;
    float best = kScales[0];
    for (const float scale : kScales)
        if (std::fabs(scale - wanted) < std::fabs(best - wanted)) best = scale;
    Settings::Get().uiScale = best;
    Settings::Get().uiScaleSaved = true;
    Settings::Save();
    Log::Writef("UI: first run: the menu's scale %.0f%% for the game's %u x %u picture (Settings > UI scale changes it)",
                best * 100.0f, s_width, s_height);
}

HRESULT PresentCommon(IDXGISwapChain* swap, UINT syncInterval, UINT flags, PresentFn originalPresent) {
    
    
    
    if (flags & DXGI_PRESENT_TEST)
        return originalPresent ? originalPresent(swap, syncInterval, flags) : E_FAIL;

    StallSampler::PresentBeat();
    // v0.9.287 frame-time probe: the framework's own work below (ticks, scripts, overlay) and the Present call.
    const auto frameworkBegin = std::chrono::steady_clock::now();
    RtProbe::OnPresent();   // (v0.9.563) the probe's capture buffers: one frame at most
    Features::SetTrainerMenuOpen(OverlayOwnsInput());
    if (!g_menuOpen)
        Features::SetInstanceBrowserActive(false);
    Features::Tick();
    Features::ServiceStartupUiHideSafety();
    Features::ServiceStartupTiming();
    ScriptEngine::Tick();
    if (!g_imguiReady) InitImGui(swap);
    ChooseFirstRunUiScale(swap);   // (v1.0.0)

    if (g_imguiReady) {
        const bool maskStartupLegal = ShouldMaskStartupLegalPresentation();
        const bool viewerFrame = !maskStartupLegal && CutsceneViewer::NeedsFrame();
        // (v0.9.563) and while the ray-traced lamp shadows are on (the probe's scene made in the overlay's frame)
        const bool needsOverlayFrame = maskStartupLegal || g_menuOpen || UI::WorldOverlayActive() || viewerFrame ||
                                       (g_rendererApi == RendererApi::DirectX12 && RtProbe::NeedsFrames());
        static bool s_viewerOwned = false;
        const bool viewerOwns = CutsceneViewer::WantsInput();
        if (g_menuOpen || viewerOwns != s_viewerOwned)
            ApplyMenuFocusState();
        s_viewerOwned = viewerOwns;

        
        
        
        
        
        if (needsOverlayFrame) {
            if (g_rendererApi == RendererApi::DirectX11) {
                
                
                
                if (maskStartupLegal && g_rtv && g_context) {
                    const FLOAT black[4] = {0.0f, 0.0f, 0.0f, 1.0f};
                    g_context->OMSetRenderTargets(1, &g_rtv, nullptr);
                    g_context->ClearRenderTargetView(g_rtv, black);
                }
                ImGui_ImplDX11_NewFrame();
            } else if (g_rendererApi == RendererApi::DirectX12) {
                Dx12Overlay::NewFrame();
            }

            ImGui_ImplWin32_NewFrame();
            
            
            if (g_menuOpen && g_menuPassThrough)
                SetCursor(nullptr);
            ImGui::NewFrame();
            ApplyLauMenuHookStyle();
            ApplyUiScaleIfNeeded();
            if (g_menuOpen) UI::Draw();
            if (!maskStartupLegal) UI::DrawWorldOverlay();
            if (viewerFrame) CutsceneViewer::Draw();
            ImGui::Render();
            // (v0.9.602) a frame with nothing in it (the Model Viewer's grid open: its frame wanted every present, the
            // grid the game's own UI, nothing of ours drawn): not submitted. It drew nothing; on DX12 it cost a command
            // list, two barriers, a submission and a signal each present, and could wait up to 250 ms for the GPU.
            // Not when the back buffer is cleared black or the ray tracing probe works in the frame.
            const ImDrawData* drawn = ImGui::GetDrawData();
            const bool clearBlack = maskStartupLegal || (viewerFrame && CutsceneViewer::FullBlack());
            const bool nothingDrawn = drawn && drawn->TotalVtxCount == 0 && !clearBlack &&
                                      !(g_rendererApi == RendererApi::DirectX12 && RtProbe::RecordsFrames());
            if (nothingDrawn) {
                ++g_emptyOverlayFrames;
            } else if (g_rendererApi == RendererApi::DirectX11) {
                if (g_rtv) {
                    
                    
                    
                    
                    g_context->OMSetRenderTargets(1, &g_rtv, nullptr);
                    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
                }
            } else if (g_rendererApi == RendererApi::DirectX12) {
                // (the viewer's full black too: the back buffer cleared, waiting for the overlay's last use of it
                // when frames come faster than the GPU gives it back, instead of skipping the frame; 0.9.387's main
                // menu load without vsync showed the loading screen through every skipped one)
                if (!Dx12Overlay::Render(swap, clearBlack)) {
                    Log::Write("Overlay: DX12 frame submission failed; renderer will retry initialization");
                    Dx12Overlay::BeforeResize();
                    g_imguiReady = false;
                }
            }
        }
    }

    
    
    
    
    
    const bool profileLoadingPresent = Features::LoadingProfilerSessionActive();
    const bool fastLoadingPresent = Features::FastLoadingRuntimeActive();
    const bool nonBlockingLoadingPresent =
        g_rendererApi == RendererApi::DirectX12 && Features::FastLoadingNonBlockingPresentRuntimeActive();
    const UINT appliedSyncInterval = (fastLoadingPresent && syncInterval > 0) ? 0u : syncInterval;

    
    
    
    
    
    
    UINT appliedPresentFlags = flags;
    bool usedDoNotWait = false;
    if (nonBlockingLoadingPresent &&
        (flags & (DXGI_PRESENT_TEST | DXGI_PRESENT_RESTART)) == 0) {
        appliedPresentFlags |= DXGI_PRESENT_DO_NOT_WAIT;
        usedDoNotWait = true;
    }

    const auto presentBegin = std::chrono::steady_clock::now();
    const HRESULT rawHr = originalPresent ? originalPresent(swap, appliedSyncInterval, appliedPresentFlags) : E_FAIL;
    {
        const auto presentDone = std::chrono::steady_clock::now();
        const auto oursUs = std::chrono::duration_cast<std::chrono::microseconds>(presentBegin - frameworkBegin).count();
        const auto callUs = std::chrono::duration_cast<std::chrono::microseconds>(presentDone - presentBegin).count();
        Features::RecordPresentTiming(oursUs > 0 ? static_cast<std::uint64_t>(oursUs) : 0ull,
                                      callUs > 0 ? static_cast<std::uint64_t>(callUs) : 0ull);
    }
    if (g_legalFirstUnmaskedPresentPending && !g_legalFirstUnmaskedPresentLogged && SUCCEEDED(rawHr)) {
        g_legalFirstUnmaskedPresentPending = false;
        g_legalFirstUnmaskedPresentLogged = true;
        const std::uint64_t firstProcessMs = Features::StartupTimingMilestoneMs(0u);
        const std::uint64_t nowMs = static_cast<std::uint64_t>(GetTickCount64());
        const std::uint64_t deltaMs = (firstProcessMs && nowMs >= firstProcessMs) ? (nowMs - firstProcessMs) : 0ull;
        const std::uint64_t legalExitMs = Features::StartupTimingMilestoneMs(4u);
        const std::uint64_t profileEnterMs = Features::StartupTimingMilestoneMs(5u);
        const std::uint64_t exitDelta = (firstProcessMs && legalExitMs >= firstProcessMs) ? (legalExitMs - firstProcessMs) : 0ull;
        const std::uint64_t profileDelta = (firstProcessMs && profileEnterMs >= firstProcessMs) ? (profileEnterMs - firstProcessMs) : 0ull;
        Log::Detailf("StartupTiming: +%llu ms | first framework-unmasked Present completed (Legal OnExit +%llu ms, PROFILE_LOAD OnEnter +%llu ms)",
                    static_cast<unsigned long long>(deltaMs),
                    static_cast<unsigned long long>(exitDelta),
                    static_cast<unsigned long long>(profileDelta));
    }
    const bool wouldBlockDrop = usedDoNotWait && rawHr == DXGI_ERROR_WAS_STILL_DRAWING;
    const bool otherFailure = usedDoNotWait && FAILED(rawHr) && !wouldBlockDrop;
    const HRESULT hr = wouldBlockDrop ? S_OK : rawHr;
    if (profileLoadingPresent) {
        const auto presentEnd = std::chrono::steady_clock::now();
        const auto elapsedNs = std::chrono::duration_cast<std::chrono::nanoseconds>(presentEnd - presentBegin).count();
        Features::RecordLoadingPresent(syncInterval, appliedSyncInterval,
            usedDoNotWait, wouldBlockDrop, otherFailure,
            elapsedNs > 0 ? static_cast<std::uint64_t>(elapsedNs) : 0ull);
    }
    return hr;
}

HRESULT __stdcall HookPresent(IDXGISwapChain* swap, UINT syncInterval, UINT flags) {
    return PresentCommon(swap, syncInterval, flags, g_originalPresent);
}

HRESULT __stdcall HookPresentAlt(IDXGISwapChain* swap, UINT syncInterval, UINT flags) {
    return PresentCommon(swap, syncInterval, flags, g_originalPresentAlt);
}

HRESULT ResizeCommon(IDXGISwapChain* swap, UINT bufferCount, UINT width, UINT height,
                     DXGI_FORMAT format, UINT swapChainFlags, ResizeBuffersFn originalResize) {
    if (g_rendererApi == RendererApi::DirectX11 && g_imguiReady) {
        ImGui_ImplDX11_InvalidateDeviceObjects();
        DestroyRtv();
    } else if (g_rendererApi == RendererApi::DirectX12 && g_imguiReady) {
        Dx12Overlay::BeforeResize();
        g_imguiReady = false;
    }

    const HRESULT hr = originalResize ? originalResize(swap, bufferCount, width, height, format, swapChainFlags) : E_FAIL;
    if (SUCCEEDED(hr)) {
        if (g_rendererApi == RendererApi::DirectX11 && g_device) {
            CreateRtv(swap);
            ImGui_ImplDX11_CreateDeviceObjects();
            g_imguiReady = true;
        } else if (g_rendererApi == RendererApi::DirectX12 && g_imguiContextReady && g_win32Ready) {
            if (Dx12Overlay::AfterResize(swap)) {
                g_rtvFormat = Dx12Overlay::RenderTargetFormat();
                g_rtvIsSrgb = Dx12Overlay::RenderTargetIsSrgb();
                g_imguiReady = true;
            }
        }
    }
    return hr;
}

HRESULT __stdcall HookResizeBuffers(IDXGISwapChain* swap, UINT bufferCount, UINT width, UINT height,
                                    DXGI_FORMAT format, UINT swapChainFlags) {
    return ResizeCommon(swap, bufferCount, width, height, format, swapChainFlags, g_originalResizeBuffers);
}

HRESULT __stdcall HookResizeBuffersAlt(IDXGISwapChain* swap, UINT bufferCount, UINT width, UINT height,
                                       DXGI_FORMAT format, UINT swapChainFlags) {
    return ResizeCommon(swap, bufferCount, width, height, format, swapChainFlags, g_originalResizeBuffersAlt);
}

LRESULT CALLBACK DummyWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    return DefWindowProcW(hwnd, msg, wp, lp);
}
}

namespace Overlay {
bool CreateHooks() {
    if (g_hooksCreated) return true;

    HINSTANCE inst = GetModuleHandleW(nullptr);
    const wchar_t* cls = L"ROTTRFrameworkDummyDXGI";
    WNDCLASSW wc{};
    wc.lpfnWndProc = DummyWndProc;
    wc.hInstance = inst;
    wc.lpszClassName = cls;
    RegisterClassW(&wc);
    HWND hwnd = CreateWindowExW(0, cls, L"", WS_OVERLAPPEDWINDOW,
                                0, 0, 100, 100, nullptr, nullptr, inst, nullptr);
    if (!hwnd) {
        Log::Write("Overlay: failed to create dummy window");
        return false;
    }

    DXGI_SWAP_CHAIN_DESC sd{};
    sd.BufferCount = 1;
    sd.BufferDesc.Width = 100;
    sd.BufferDesc.Height = 100;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = hwnd;
    sd.SampleDesc.Count = 1;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    IDXGISwapChain* swap = nullptr;
    ID3D11Device* device = nullptr;
    ID3D11DeviceContext* context = nullptr;
    D3D_FEATURE_LEVEL level{};
    const D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0 };

    const HRESULT hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0,
        levels, static_cast<UINT>(std::size(levels)), D3D11_SDK_VERSION, &sd,
        &swap, &device, &level, &context);

    if (FAILED(hr) || !swap) {
        if (context) context->Release();
        if (device) device->Release();
        DestroyWindow(hwnd);
        UnregisterClassW(cls, inst);
        Log::Write("Overlay: failed to create dummy D3D11 swap chain");
        return false;
    }

    void** vtable = *reinterpret_cast<void***>(swap);
    void* present = vtable[8];
    void* resize = vtable[13];

    bool ok = true;
    if (MH_CreateHook(present, reinterpret_cast<void*>(&HookPresent),
                      reinterpret_cast<void**>(&g_originalPresent)) != MH_OK)
        ok = false;
    if (MH_CreateHook(resize, reinterpret_cast<void*>(&HookResizeBuffers),
                      reinterpret_cast<void**>(&g_originalResizeBuffers)) != MH_OK)
        ok = false;


    
    
    context->Release();
    device->Release();
    swap->Release();

    void* dx12Present = nullptr;
    void* dx12Resize = nullptr;
    Dx12Overlay::CreateHookTargets(hwnd, &dx12Present, &dx12Resize);

    if (dx12Present && dx12Present != present) {
        const MH_STATUS st = MH_CreateHook(dx12Present, reinterpret_cast<void*>(&HookPresentAlt),
                                           reinterpret_cast<void**>(&g_originalPresentAlt));
        if (st == MH_OK)
            Log::Detail("Overlay: distinct DX12 DXGI Present thunk hooked");
        else
            Log::Writef("Overlay: distinct DX12 Present hook failed (MinHook status=%d)", static_cast<int>(st));
    }
    if (dx12Resize && dx12Resize != resize) {
        const MH_STATUS st = MH_CreateHook(dx12Resize, reinterpret_cast<void*>(&HookResizeBuffersAlt),
                                           reinterpret_cast<void**>(&g_originalResizeBuffersAlt));
        if (st == MH_OK)
            Log::Detail("Overlay: distinct DX12 DXGI ResizeBuffers thunk hooked");
        else
            Log::Writef("Overlay: distinct DX12 ResizeBuffers hook failed (MinHook status=%d)", static_cast<int>(st));
    }

    DestroyWindow(hwnd);
    UnregisterClassW(cls, inst);

    g_hooksCreated = ok;
    if (ok) {
        Log::Detail(Dx12Overlay::QueueCaptureHookCreated()
            ? "Overlay: unified DX11/DX12 Present/ResizeBuffers hooks created"
            : "Overlay: Present/ResizeBuffers hooks created (DX12 queue capture unavailable)");
    } else {
        Log::Write("Overlay: Present/ResizeBuffers hook creation failed");
    }
    return ok;
}

bool HooksCreated() { return g_hooksCreated; }

// (v0.9.602) Presents whose overlay frame drew nothing (not submitted) since the last call.
std::uint32_t TakeEmptyFrames() { return g_emptyOverlayFrames.exchange(0); }

const char* RendererName() {
    switch (g_rendererApi) {
    case RendererApi::DirectX11: return "DirectX 11";
    case RendererApi::DirectX12: return "DirectX 12";
    default: return "Detecting";
    }
}

const char* RendererStatus() {
    if (g_rendererApi == RendererApi::DirectX12)
        return Dx12Overlay::Status();
    if (g_rendererApi == RendererApi::DirectX11)
        return g_imguiReady ? "DX11 ImGui renderer ready" : "waiting for DX11 swap-chain initialization";
    return "waiting for game swap chain";
}

bool PlayerInputLocked() {
    return g_playerInputLocked;
}

bool OwnsInput() {
    return OverlayOwnsInput();
}

bool PlayerInputLockedDuringFreeCamera() {
    return g_playerInputLocked;
}

bool SetFreeCameraEnabled(bool value) {
    const bool wasEnabled = Features::FreeCameraEnabled();
    if (!Features::SetFreeCamera(value))
        return false;

    const bool nowEnabled = Features::FreeCameraEnabled();
    if (nowEnabled == wasEnabled)
        return true;

    if (nowEnabled) {
        if (!g_playerInputLocked) {
            g_playerInputLocked = true;
            g_freeCameraAutoLockedPlayerInput = true;
        } else {
            g_freeCameraAutoLockedPlayerInput = false;
        }
    } else {
        if (g_freeCameraAutoLockedPlayerInput)
            g_playerInputLocked = false;
        g_freeCameraAutoLockedPlayerInput = false;
    }
    return true;
}

void Shutdown() {
    if (g_rendererApi == RendererApi::DirectX11 && g_imguiReady)
        ImGui_ImplDX11_Shutdown();

    
    
    Dx12Overlay::Shutdown();
    g_imguiReady = false;

    if (g_oldWndProc && g_hwnd) {
        SetWindowLongPtrW(g_hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(g_oldWndProc));
        g_oldWndProc = nullptr;
    }
    if (g_win32Ready) {
        ImGui_ImplWin32_Shutdown();
        g_win32Ready = false;
    }
    if (g_imguiContextReady) {
        ImGui::DestroyContext();
        g_imguiContextReady = false;
        g_appliedUiScale = 1.0f;
        g_appliedUiTheme = -1;
        g_styleApplied = false;
    }

    DestroyRtv();
    if (g_context) { g_context->Release(); g_context = nullptr; }
    if (g_device) { g_device->Release(); g_device = nullptr; }
    g_rendererApi = RendererApi::Unknown;
}
}
