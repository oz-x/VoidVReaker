#pragma once
#include <string>

namespace vvr {

struct Config {
    bool vr_enabled = true;          // master switch
    float world_scale = 0.0f;        // Unreal units per metre; 0 = the level's WorldToMeters
    bool positional = true;          // apply 6DoF head position
    float resolution_scale = 0.75f;  // multiplier on the runtime's recommended per-eye size
    int eye_width = 0;               // 0 = runtime recommendation x resolution_scale
    int eye_height = 0;
    int aim_mode = 2;                // 0 = game (stick/mouse), 1 = head, 2 = right controller
    float turn_speed = 180.0f;       // smooth turning with the right stick, degrees per second
    float snap_turn = 0.0f;          // > 0: snap turning by this many degrees instead
    bool reticle = true;             // sight where the gun points
    // Weapon in the right hand: the point of the game's first-person view (cm: forward, right,
    // up from the camera) that is moved onto the controller, and an extra pitch in degrees.
    float gun_x = 33.0f;
    float gun_y = 12.0f;
    float gun_z = -18.0f;
    float gun_pitch = 0.0f;
    float gun_yaw = 5.0f;            // degrees, positive turns the gun to the right
    float arms_max_offset = 0.0f;    // cm the arms may move from the game's place (0 = no limit)
    bool hide_arms = true;           // show only the gun in the hand
    bool mirror = true;              // show the left eye on the desktop window
    bool menus_on_screen = true;     // menus on the virtual screen
    float screen_distance = 2.2f;    // virtual screen, metres in front of you
    float screen_width = 2.6f;       // metres
    int recenter_key = 0x75;         // VK_F6
    int predict_frames = 2;          // pose prediction, frames ahead of the presented one
    // Debug
    bool fake_xr = false;            // stereo pipeline without a headset (eye dumps)
    // Bisection aid: 1 AdjustViewRect, 2 CalculateStereoViewOffset, 4 projection,
    // 8 UI out of the eyes, 16 stereo target size, 32 plain 2D stereo target, 0x100 swap eyes.
    int stereo_patches = 0x3F;
    bool trace = false;              // file triggers and periodic status lines
    bool d3d_debug = false;          // D3D12 debug layer (validation messages in the log)
    unsigned eye_source_state = 0x4;  // D3D12 state of the eye targets at Present (RTV: RDG AccessFinal)
    float fake_yaw = 0.0f;           // fake headset orientation, degrees (positive = left)
    float fake_pitch = 0.0f;         // degrees (positive = up)
    float fake_hand_yaw = 0.0f;      // fake right controller orientation, degrees
    float fake_eye_split = 0.0f;     // extra yaw of the fake right eye (tells the eyes apart)
    float fake_hand_pitch = 0.0f;
    bool fake_fire = false;          // pulse the fake right trigger
    std::wstring extra_args;         // appended to the game's command line
    std::wstring dir;                // Binaries\Win64 directory, with trailing backslash
};

const Config& config();
void config_load();

}  // namespace vvr
