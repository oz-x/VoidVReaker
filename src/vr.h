#pragma once
#include <d3d12.h>

namespace vvr {

// DllMain: config, log, command line and DXGI hooks.
void vr_early_init();
// First DXGI factory creation (main thread, engine starting): OpenXR instance and engine hooks.
void vr_engine_init();
// The game's swapchain exists: its device and presenting queue.
void vr_on_swapchain(ID3D12Device* device, ID3D12CommandQueue* queue);
// Headset view poses are being rendered (false: flat or no headset).
bool vr_active();

}  // namespace vvr
