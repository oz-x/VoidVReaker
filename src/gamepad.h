#pragma once
#include "xr.h"

// Presents the VR motion controllers to the game as an Xbox gamepad (XInput user 0), so every
// action works with the game's own gamepad bindings, in menus too.
namespace vvr {

bool gamepad_install();
// Game thread, once per frame. `move_rotation` (radians, Unreal yaw sense) rotates the left
// stick so that walking follows the head instead of the aiming hand. The right stick turns
// in the game only in menus or when `game_turns` (otherwise the mod turns the body itself).
void gamepad_publish(const xr::ControllerInput& input, float move_rotation, bool in_menu, bool game_turns);
// Game rumble requested through XInputSetState, 0..1 per motor (left, right).
void gamepad_rumble(float* left, float* right);
// Debug: holds buttons/axes for a while ("A", "DOWN 200", "LX 0 LY 1 800").
void gamepad_debug_press(const char* text);

}  // namespace vvr
