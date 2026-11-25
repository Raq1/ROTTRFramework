#pragma once
#include <cstdint>

// DLAA: DLSS at the screen's own resolution (anti-aliasing only, no upscaling). v0.9.484: an F8 checkbox for every
// DLSS quality; v0.9.485: its own entry in Options > Display > DLSS, switched like the others (no restart).
//
// The game's DLSS is Nixxes' NxDLSSImpl (nx layer, statically linked NGX SDK; nvngx_dlss.dll next to ROTTR.exe). Its
// setting is DisplaySettings +0x90 (0 off, 1 Performance, 2 Balanced, 3 Quality, 4 Ultra Performance; saved by
// NxSettingsImpl as the registry DWORD "DLSS", no range check on load, none in NxAppImpl::ValidateSettings). DLAA is a
// new value, 5:
// - The list (UiMenuStateNixxesOptionsScreenDisplay::FillDLSSOption, TR2 0x92D1A0, retail 0x8EA410) is built from five
//   labels in the order of the values, and its selected index is the value (item data +0x40 = settings +0x90; the
//   labels in the cdc::Array<ItemValue> at item data +0x48: ItemValue 0x20 = UTF8String {u32 chars; Array<char>
//   {count, capacity, data}} + int value (-1) + u8). Hooked: after the game's five, a "DLAA" entry is added (the game's
//   push_back copies it) and the six put in the order Off, DLAA, Quality, Balanced, Performance, Ultra Performance; the
//   selected index is the value's place in it.
// - A step on it (OnOptionStepperValueChanged, TR2 0x96A8A0, retail 0x926AC0: GFx args[0] the option id, 5 for DLSS,
//   args[1] the new index) writes the index into the pending settings' +0x90 and rebuilds the list (UpdateItems ->
//   FillDLSSOption); the index is turned into its value there (or right after, if the list was not rebuilt).
// - The render size: NxDLSSImpl::ConvertScreenToRenderRes (vtable +0x28; TR2 0xD00950, retail 0xCB52C0): (this, uint&
//   width, uint& height, uint& maxWidth, uint& maxHeight, uint& minWidth, uint& minHeight, DLSSQuality); in: the
//   screen size; it asks NGX's DLSSOptimalSettingsCallback (PerfQualityValue: the game's 1 -> 0 MaxPerf, 2 -> 1
//   Balanced, 3 -> 2 MaxQuality, 4 -> 3 UltraPerformance, else 1) and returns OutWidth / OutHeight and the dynamic
//   max / min render sizes. Called by NxSwapChainImpl::ComputeScaledGameRenderRect (TR2 0xCFE970) while DLSS is on,
//   from NxSwapChainImpl::Create (the start) and ApplySettings (Options > Display). For 5 the game's call is made as
//   Quality (3) and the six outputs then set to the screen size.
// - The DLSS feature: NxDLSSImpl::UpdateState (TR2 0xD02830, retail 0xCB7140; every frame) maps the setting (read
//   through NxAppImpl::GetDisplaySettings, vtable +0xF0, retail 0xC82660: lea rax, [rcx+51Ch]) to NGX's quality and
//   creates the feature again (InitDLSS, retail 0xCB59C0, which maps it once more and stores it at +0x1C) when that
//   quality (+0x1C), the output size (+0x10 / +0x14) or HDR (+0x18) changed; the input size is not compared. For those
//   two reads (return addresses retail 0xCB71B3 and 0xCB5A05; and ResolutionSupportsDLSS's, 0xCB69D4, the display
//   modes DLSS supports) GetDisplaySettings answers a copy that says Quality, so
//   DLAA runs NGX's MaxQuality as 0.9.484 did; and when the setting goes to or from DLAA, UpdateState finds +0x1C set
//   to -1 first, so the feature is made again for the new input size.
// Without the framework the game reads a saved 5 as Balanced (and its list shows no entry for it).
namespace Dlaa {
constexpr std::uint32_t kDlssDlaa = 5;   // DisplaySettings +0x90

bool Install();   // the hooks (early startup group, before the swap chain is made); false: unavailable (logged)
bool Available();
bool ListInstalled();   // the DLAA entry in Options > Display

struct LastSetup {
    bool seen = false;            // DLSS set up since the start
    bool dlaa = false;            // as DLAA
    std::uint32_t screenWidth = 0, screenHeight = 0;
    std::uint32_t renderWidth = 0, renderHeight = 0;
    int quality = 0;              // the game's DLSS setting (1..5)
};
LastSetup Last();
}  // namespace Dlaa
