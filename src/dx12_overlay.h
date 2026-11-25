#pragma once

#include <Windows.h>
#include <dxgi.h>
#include <dxgiformat.h>
#include <cstdint>

struct ID3D12CommandQueue;   // (v0.9.561)

namespace Dx12Overlay {




bool CreateHookTargets(HWND dummyWindow, void** outPresent, void** outResizeBuffers);
bool QueueCaptureHookCreated();
bool QueueCaptured();


void RetireQueueCaptureHooks();





bool PrimeQueueCaptureForSwapDevice(IDXGISwapChain* swapChain);



bool TryInitialize(IDXGISwapChain* swapChain);
bool Ready();




bool GetTexturePreviewId(std::uintptr_t textureAddress, std::uint64_t* outTextureId);
// (v0.9.543) A descriptor of the overlay's shader-visible heap (CPU and GPU handles' values), kept until the overlay
// lets its resources go (RtProbe::OnOverlayReleased is told)
bool AllocateDescriptor(std::uint64_t* cpu, std::uint64_t* gpu);
// (v0.9.555) The overlay's queue's timestamp frequency (ticks a second; 0: none)
std::uint64_t TimestampFrequency();
// (v0.9.561) The game's queue the overlay draws on (null until captured)
ID3D12CommandQueue* Queue();
void NewFrame();
bool Render(IDXGISwapChain* swapChain, bool clearBackBufferBlack);
void BeforeResize();
bool AfterResize(IDXGISwapChain* swapChain);
void Shutdown();

DXGI_FORMAT RenderTargetFormat();
bool RenderTargetIsSrgb();
const char* Status();
}
