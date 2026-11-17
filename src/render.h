#pragma once
#include <cstdint>

// D3D12/DXGI side: swapchain hooks, eye targets and UI capture, OpenXR submission at Present,
// desktop mirror and debug dumps.
namespace vvr {

// Hooks DXGI factory creation (called from DllMain).
bool render_install();
// Before the engine creates its D3D12 device (D3DDebug=1).
void render_enable_debug_layer();
// Render thread, once per Slate frame of the game window: the engine textures (FRHITexture)
// holding this frame's left and right eye (searched for their ID3D12Resource).
void render_on_frame_targets(void* left, void* right);
void render_request_dump();
void render_backbuffer_size(uint32_t* width, uint32_t* height);

}  // namespace vvr
