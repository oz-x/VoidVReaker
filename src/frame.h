#pragma once
#include <vector>

#include "xr.h"

namespace vvr {

// What the game thread decided for one frame; travels with the frame to Present.
struct FrameSlot {
    bool stereo = false;       // the eyes were rendered with `views`
    XrView views[2]{};         // LOCAL space poses / fovs
    bool screen = false;       // show the whole UI on the virtual screen
    XrPosef screen_pose{};
    float screen_width = 2.6f; // metres
    std::vector<xr::Quad> hud; // HUD pieces (rects of the UI image) placed in the world
};

// Game thread, once per drawn frame (before the engine renders it).
void frame_queue_push(const FrameSlot& slot);

}  // namespace vvr
