#pragma once
#include <cstdint>

#include "mathx.h"

// Drives the engine's own stereo renderer (FFakeStereoRenderingDevice, enabled with
// -emulatestereo): per-eye cameras and projections from the headset, eye target size, and the
// end-of-frame copy of the stereo target to the window.
namespace vvr {

// Head-relative eye data for the frame being drawn (Unreal axes, metres, relative to the
// recentred reference yaw).
struct EyeFrame {
    bool valid = false;
    mathx::Vec3 offset[2];               // eye position relative to the reference head
    mathx::Vec3 forward[2], right[2], up[2];
    double tan_left[2]{}, tan_right[2]{}, tan_up[2]{}, tan_down[2]{};
    bool body_yaw_valid = false;         // use body_yaw instead of the camera's yaw
    double body_yaw = 0.0;               // degrees
};

bool stereo_device_install();
void stereo_set_enabled(bool enabled);
bool stereo_enabled();
// The viewport is drawn once per eye (game thread): the eye the next draw renders and the
// stereo view index that carries it (the other view is left out); -1 draws both views.
void stereo_set_draw_eye(int eye, int view_index);
void stereo_set_eye_size(uint32_t width, uint32_t height);
void stereo_eye_size(uint32_t* width, uint32_t* height);
// Game thread, before the engine draws the frame.
void stereo_set_frame(const EyeFrame& frame);
// Units per metre used for the last eye placement (engine WorldToMeters or the configured scale).
double stereo_world_scale();

}  // namespace vvr
