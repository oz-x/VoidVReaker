#include "config.h"

#include <windows.h>

#include <algorithm>
#include <cstdlib>

namespace vvr {

namespace {
Config g_config;

float read_float(const wchar_t* ini, const wchar_t* section, const wchar_t* key, float fallback) {
    wchar_t buf[64];
    GetPrivateProfileStringW(section, key, L"", buf, 64, ini);
    if (buf[0] == 0) {
        return fallback;
    }
    return static_cast<float>(_wtof(buf));
}

int read_int(const wchar_t* ini, const wchar_t* section, const wchar_t* key, int fallback) {
    wchar_t buf[64];
    GetPrivateProfileStringW(section, key, L"", buf, 64, ini);
    if (buf[0] == 0) {
        return fallback;
    }
    return static_cast<int>(wcstol(buf, nullptr, 0));  // accepts 0x.. for keys and states
}

bool read_bool(const wchar_t* ini, const wchar_t* section, const wchar_t* key, bool fallback) {
    return read_int(ini, section, key, fallback ? 1 : 0) != 0;
}

}  // namespace

const Config& config() {
    return g_config;
}

void config_load() {
    wchar_t path[MAX_PATH];
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    std::wstring dir = path;
    dir.resize(dir.find_last_of(L"\\/") + 1);
    g_config.dir = dir;

    const std::wstring ini_path = dir + L"voidvr.ini";
    const wchar_t* ini = ini_path.c_str();
    Config& c = g_config;
    c.vr_enabled = read_bool(ini, L"VR", L"Enabled", c.vr_enabled);
    c.world_scale = read_float(ini, L"VR", L"WorldScale", c.world_scale);
    c.positional = read_bool(ini, L"VR", L"PositionalTracking", c.positional);
    c.resolution_scale = std::clamp(read_float(ini, L"VR", L"ResolutionScale", c.resolution_scale),
                                    0.3f, 2.5f);
    c.eye_width = read_int(ini, L"VR", L"EyeWidth", c.eye_width);
    c.eye_height = read_int(ini, L"VR", L"EyeHeight", c.eye_height);
    c.aim_mode = std::clamp(read_int(ini, L"VR", L"AimMode", c.aim_mode), 0, 2);
    c.turn_speed = read_float(ini, L"VR", L"TurnSpeed", c.turn_speed);
    c.snap_turn = read_float(ini, L"VR", L"SnapTurn", c.snap_turn);
    c.reticle = read_bool(ini, L"VR", L"Reticle", c.reticle);
    c.gun_x = read_float(ini, L"VR", L"GunOffsetForward", c.gun_x);
    c.gun_y = read_float(ini, L"VR", L"GunOffsetRight", c.gun_y);
    c.gun_z = read_float(ini, L"VR", L"GunOffsetUp", c.gun_z);
    c.gun_pitch = read_float(ini, L"VR", L"GunPitch", c.gun_pitch);
    c.gun_yaw = read_float(ini, L"VR", L"GunYaw", c.gun_yaw);
    c.arms_max_offset = read_float(ini, L"VR", L"ArmsMaxOffset", c.arms_max_offset);
    c.hide_arms = read_bool(ini, L"VR", L"HideArms", c.hide_arms);
    c.mirror = read_bool(ini, L"VR", L"DesktopMirror", c.mirror);
    c.menus_on_screen = read_bool(ini, L"VR", L"MenusOnScreen", c.menus_on_screen);
    c.screen_distance = read_float(ini, L"VR", L"ScreenDistance", c.screen_distance);
    c.screen_width = read_float(ini, L"VR", L"ScreenWidth", c.screen_width);
    c.recenter_key = read_int(ini, L"Keys", L"Recenter", c.recenter_key);
    c.predict_frames = std::clamp(read_int(ini, L"VR", L"PredictFrames", c.predict_frames), 0, 6);
    c.fake_xr = read_bool(ini, L"Debug", L"FakeHeadset", c.fake_xr);
    c.trace = read_bool(ini, L"Debug", L"Trace", c.trace);
    c.d3d_debug = read_bool(ini, L"Debug", L"D3DDebug", c.d3d_debug);
    c.stereo_patches = read_int(ini, L"Debug", L"StereoPatches", c.stereo_patches);
    c.eye_source_state = static_cast<unsigned>(
        read_int(ini, L"Debug", L"EyeSourceState", static_cast<int>(c.eye_source_state)));
    c.fake_yaw = read_float(ini, L"Debug", L"FakeYaw", c.fake_yaw);
    c.fake_pitch = read_float(ini, L"Debug", L"FakePitch", c.fake_pitch);
    c.fake_hand_yaw = read_float(ini, L"Debug", L"FakeHandYaw", c.fake_hand_yaw);
    c.fake_eye_split = read_float(ini, L"Debug", L"FakeEyeSplit", c.fake_eye_split);
    c.fake_hand_pitch = read_float(ini, L"Debug", L"FakeHandPitch", c.fake_hand_pitch);
    c.fake_fire = read_bool(ini, L"Debug", L"FakeFire", c.fake_fire);
    wchar_t extra[1024] = {};
    GetPrivateProfileStringW(L"Debug", L"ExtraArgs", L"", extra, 1024, ini);
    c.extra_args = extra;
}

}  // namespace vvr
