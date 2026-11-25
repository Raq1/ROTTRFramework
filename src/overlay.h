#pragma once
#include <cstdint>
namespace Overlay {
bool CreateHooks();
bool HooksCreated();
bool PlayerInputLocked();
bool OwnsInput();   // (v0.9.486) the F8 menu (not pass-through) or the Cutscene Viewer takes the keyboard
bool PlayerInputLockedDuringFreeCamera(); 
bool SetFreeCameraEnabled(bool value);
std::uint32_t TakeEmptyFrames();   // (v0.9.602) presents whose overlay frame drew nothing (not submitted) since the last call
const char* RendererName();
const char* RendererStatus();
void Shutdown();
}
