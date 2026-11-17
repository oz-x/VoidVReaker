#pragma once
#include <windows.h>
#include <d3d12.h>

#include <cstdint>
#include <vector>

#ifndef XR_USE_PLATFORM_WIN32
#define XR_USE_PLATFORM_WIN32
#endif
#ifndef XR_USE_GRAPHICS_API_D3D12
#define XR_USE_GRAPHICS_API_D3D12
#endif
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

namespace vvr::xr {

// Instance + system. Returns false when no runtime/headset is available.
bool init_instance();
bool available();
bool adapter_luid(LUID* out);
bool create_session(ID3D12Device* device, ID3D12CommandQueue* queue);
bool session_running();
// True once after the runtime recentred its LOCAL space (headset recenter gesture).
bool consume_recenter_request();
// Per-eye size recommended by the runtime.
uint32_t recommended_width();
uint32_t recommended_height();

// ---- game thread ----
void poll_events();
// Display time to predict poses for when sampling them now (frames ahead of the one being
// presented, see Config::predict_frames).
XrTime predicted_time();
bool locate_views(XrTime time, XrView views[2]);

// ---- presenting thread ----
// A piece of the UI image placed in the LOCAL space.
struct Quad {
    XrPosef pose{};
    XrExtent2Df size{};  // metres
    XrRect2Di rect{};    // pixels of the source image
    bool marker = false; // source: the mod's waypoint pin (kMarkerSize square) instead of the UI
};
constexpr int32_t kMarkerSize = 128;

struct Submission {
    // The engine's eye targets (left, right), both eye_width x eye_height.
    ID3D12Resource* eyes[2]{};
    D3D12_RESOURCE_STATES eyes_state = D3D12_RESOURCE_STATE_COMMON;
    uint32_t eye_width = 0;
    uint32_t eye_height = 0;
    XrView views[2]{};  // poses/fovs the eyes were rendered with
    // Game UI over transparent black (the window back buffer).
    ID3D12Resource* ui = nullptr;
    D3D12_RESOURCE_STATES ui_state = D3D12_RESOURCE_STATE_COMMON;
    uint32_t ui_width = 0;
    uint32_t ui_height = 0;
    std::vector<Quad> quads;
};

// Paces with xrWaitFrame, begins the frame, copies the images and ends the frame. No-op (and
// returns false) when the session is not running.
bool submit_frame(const Submission& submission);

// ---- motion controllers (xr_input.cpp) ----
enum Hand { kHandLeft = 0, kHandRight = 1 };

struct HandInput {
    bool pose_valid = false;
    XrPosef aim{};             // aim pose in the LOCAL space
    float trigger = 0.0f;
    float grip = 0.0f;
    XrVector2f stick{};
    bool stick_click = false;
    bool primary = false;      // A (right) / X (left)
    bool secondary = false;    // B (right) / Y (left)
    bool menu = false;         // left hand only
};

struct ControllerInput {
    bool active = false;       // actions synced while the session had focus
    HandInput hand[2];
};

// Creates the action set; called by create_session.
bool input_init(XrInstance instance, XrSession session, XrSpace local);
// Game thread: syncs actions and locates both hands at `time`.
void input_sync(XrTime time, ControllerInput& out);
// Vibrates a controller (amplitude 0..1, 0 stops). Game thread.
void haptic(Hand hand, float amplitude);

}  // namespace vvr::xr
