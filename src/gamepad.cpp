#include "gamepad.h"

#include <windows.h>
#include <Xinput.h>

#include <MinHook.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <mutex>

#include "log.h"

namespace vvr {

namespace {

using GetStateFn = DWORD(WINAPI*)(DWORD, XINPUT_STATE*);
using SetStateFn = DWORD(WINAPI*)(DWORD, XINPUT_VIBRATION*);
GetStateFn g_get_state = nullptr;
SetStateFn g_set_state = nullptr;

std::mutex g_mutex;
bool g_active = false;  // VR controllers are driving user 0
XINPUT_GAMEPAD g_pad{};
DWORD g_packet = 0;
WORD g_rumble[2] = {0, 0};
XINPUT_GAMEPAD g_debug_pad{};
DWORD g_debug_until = 0;

SHORT axis(float v) {
    return static_cast<SHORT>(std::lround(std::clamp(v, -1.0f, 1.0f) * 32767.0f));
}

BYTE trigger(float v) {
    return static_cast<BYTE>(std::lround(std::clamp(v, 0.0f, 1.0f) * 255.0f));
}

// The engine marks a controller connected only when it scans for devices (startup and device
// changes), before the headset's controllers are active: the virtual pad is reported connected
// from the start, merged with a physical pad on the same slot if there is one.
DWORD WINAPI hook_get_state(DWORD user, XINPUT_STATE* state) {
    if (user != 0 || state == nullptr) {
        return g_get_state(user, state);
    }
    XINPUT_STATE physical{};
    const bool has_physical = g_get_state(user, &physical) == ERROR_SUCCESS;
    std::lock_guard lock(g_mutex);
    static DWORD packet = 0;
    *state = physical;
    if (g_active) {
        XINPUT_GAMEPAD& pad = state->Gamepad;
        pad.wButtons |= g_pad.wButtons;
        pad.bLeftTrigger = std::max(pad.bLeftTrigger, g_pad.bLeftTrigger);
        pad.bRightTrigger = std::max(pad.bRightTrigger, g_pad.bRightTrigger);
        if (std::abs(g_pad.sThumbLX) + std::abs(g_pad.sThumbLY) > std::abs(pad.sThumbLX) + std::abs(pad.sThumbLY)) {
            pad.sThumbLX = g_pad.sThumbLX;
            pad.sThumbLY = g_pad.sThumbLY;
        }
        if (std::abs(g_pad.sThumbRX) + std::abs(g_pad.sThumbRY) > std::abs(pad.sThumbRX) + std::abs(pad.sThumbRY)) {
            pad.sThumbRX = g_pad.sThumbRX;
            pad.sThumbRY = g_pad.sThumbRY;
        }
    }
    // A packet number that changes whenever the merged state does.
    static XINPUT_GAMEPAD last{};
    if (memcmp(&last, &state->Gamepad, sizeof(last)) != 0) {
        last = state->Gamepad;
        ++packet;
    }
    state->dwPacketNumber = packet + (has_physical ? physical.dwPacketNumber : 0);
    return ERROR_SUCCESS;
}

DWORD WINAPI hook_set_state(DWORD user, XINPUT_VIBRATION* vibration) {
    if (user == 0 && vibration != nullptr) {
        std::lock_guard lock(g_mutex);
        g_rumble[0] = vibration->wLeftMotorSpeed;
        g_rumble[1] = vibration->wRightMotorSpeed;
        if (g_active) {
            return ERROR_SUCCESS;  // the headset's controllers vibrate instead
        }
    }
    return g_set_state(user, vibration);
}

bool hook(HMODULE module, const char* name, void* detour, void** original) {
    void* target = reinterpret_cast<void*>(GetProcAddress(module, name));
    return target != nullptr && MH_CreateHook(target, detour, original) == MH_OK &&
           MH_EnableHook(target) == MH_OK;
}

}  // namespace

bool gamepad_install() {
    // The game delay-loads XInputGetState / XInputSetState from xinput1_4.dll.
    HMODULE module = GetModuleHandleW(L"xinput1_4.dll");
    if (module == nullptr) {
        module = LoadLibraryW(L"xinput1_4.dll");
    }
    const bool ok = module != nullptr &&
                    hook(module, "XInputGetState", reinterpret_cast<void*>(&hook_get_state),
                         reinterpret_cast<void**>(&g_get_state)) &&
                    hook(module, "XInputSetState", reinterpret_cast<void*>(&hook_set_state),
                         reinterpret_cast<void**>(&g_set_state));
    LOG(ok ? "Virtual gamepad ready" : "ERROR: could not hook XInput");
    return ok;
}

void gamepad_publish(const xr::ControllerInput& input, float move_rotation, bool in_menu, bool game_turns) {
    XINPUT_GAMEPAD pad{};
    const bool active = input.active;
    if (active) {
        const xr::HandInput& left = input.hand[xr::kHandLeft];
        const xr::HandInput& right = input.hand[xr::kHandRight];

        // Left stick: walk relative to the head. XInput: X = right, Y = forward.
        const float c = std::cos(move_rotation), s = std::sin(move_rotation);
        const float forward = left.stick.y, strafe = left.stick.x;
        pad.sThumbLX = axis(forward * s + strafe * c);
        pad.sThumbLY = axis(forward * c - strafe * s);
        // Right stick: the game turns only in menus (scrolling, previews) or without controller
        // aiming; otherwise the mod turns the body itself. Up/down only in menus.
        pad.sThumbRX = in_menu || game_turns ? axis(right.stick.x) : 0;
        pad.sThumbRY = in_menu ? axis(right.stick.y) : 0;
        pad.bLeftTrigger = trigger(left.trigger);
        pad.bRightTrigger = trigger(right.trigger);  // fire
        if (left.grip > 0.6f) pad.wButtons |= XINPUT_GAMEPAD_LEFT_SHOULDER;
        if (right.grip > 0.6f) pad.wButtons |= XINPUT_GAMEPAD_RIGHT_SHOULDER;
        if (right.primary) pad.wButtons |= XINPUT_GAMEPAD_A;
        if (right.secondary) pad.wButtons |= XINPUT_GAMEPAD_B;
        if (left.primary) pad.wButtons |= XINPUT_GAMEPAD_X;
        if (left.secondary) pad.wButtons |= XINPUT_GAMEPAD_Y;
        if (left.stick_click) pad.wButtons |= XINPUT_GAMEPAD_LEFT_THUMB;
        if (right.stick_click) pad.wButtons |= XINPUT_GAMEPAD_RIGHT_THUMB;
        if (left.menu) pad.wButtons |= XINPUT_GAMEPAD_START;
    }
    std::lock_guard lock(g_mutex);
    // Debug presses (voidvr_pad.txt) drive the pad even without controllers.
    const bool debug = GetTickCount() < g_debug_until;
    if (debug) {
        pad.wButtons |= g_debug_pad.wButtons;
        pad.bLeftTrigger = std::max(pad.bLeftTrigger, g_debug_pad.bLeftTrigger);
        pad.bRightTrigger = std::max(pad.bRightTrigger, g_debug_pad.bRightTrigger);
        if (g_debug_pad.sThumbLX || g_debug_pad.sThumbLY) {
            pad.sThumbLX = g_debug_pad.sThumbLX;
            pad.sThumbLY = g_debug_pad.sThumbLY;
        }
        if (g_debug_pad.sThumbRX || g_debug_pad.sThumbRY) {
            pad.sThumbRX = g_debug_pad.sThumbRX;
            pad.sThumbRY = g_debug_pad.sThumbRY;
        }
    }
    const bool on = active || debug;
    if (on != g_active || memcmp(&pad, &g_pad, sizeof(pad)) != 0) {
        ++g_packet;
    }
    g_active = on;
    g_pad = pad;
}

void gamepad_debug_press(const char* text) {
    // "A", "DOWN 200", "LX 0 LY 1 500"...: buttons and axes, then the duration in ms.
    static const struct {
        const char* name;
        WORD bit;
    } kButtons[] = {{"A", XINPUT_GAMEPAD_A},           {"B", XINPUT_GAMEPAD_B},
                    {"X", XINPUT_GAMEPAD_X},           {"Y", XINPUT_GAMEPAD_Y},
                    {"LB", XINPUT_GAMEPAD_LEFT_SHOULDER}, {"RB", XINPUT_GAMEPAD_RIGHT_SHOULDER},
                    {"START", XINPUT_GAMEPAD_START},   {"BACK", XINPUT_GAMEPAD_BACK},
                    {"UP", XINPUT_GAMEPAD_DPAD_UP},    {"DOWN", XINPUT_GAMEPAD_DPAD_DOWN},
                    {"LEFT", XINPUT_GAMEPAD_DPAD_LEFT}, {"RIGHT", XINPUT_GAMEPAD_DPAD_RIGHT},
                    {"L3", XINPUT_GAMEPAD_LEFT_THUMB}, {"R3", XINPUT_GAMEPAD_RIGHT_THUMB}};
    XINPUT_GAMEPAD pad{};
    DWORD duration = 150;
    char buf[256];
    strncpy_s(buf, text, _TRUNCATE);
    char* context = nullptr;
    for (char* tok = strtok_s(buf, " \t\r\n", &context); tok != nullptr;
         tok = strtok_s(nullptr, " \t\r\n", &context)) {
        bool known = false;
        for (const auto& b : kButtons) {
            if (_stricmp(tok, b.name) == 0) {
                pad.wButtons |= b.bit;
                known = true;
            }
        }
        if (known) {
            continue;
        }
        auto value = [&]() {
            char* next = strtok_s(nullptr, " \t\r\n", &context);
            return next != nullptr ? static_cast<float>(atof(next)) : 0.0f;
        };
        if (_stricmp(tok, "LT") == 0) pad.bLeftTrigger = 255;
        else if (_stricmp(tok, "RT") == 0) pad.bRightTrigger = 255;
        else if (_stricmp(tok, "LX") == 0) pad.sThumbLX = axis(value());
        else if (_stricmp(tok, "LY") == 0) pad.sThumbLY = axis(value());
        else if (_stricmp(tok, "RX") == 0) pad.sThumbRX = axis(value());
        else if (_stricmp(tok, "RY") == 0) pad.sThumbRY = axis(value());
        else duration = static_cast<DWORD>(atoi(tok));
    }
    std::lock_guard lock(g_mutex);
    g_debug_pad = pad;
    g_debug_until = GetTickCount() + duration;
    LOG("Debug pad: buttons 0x%04X for %lu ms", pad.wButtons, duration);
}

void gamepad_rumble(float* left, float* right) {
    std::lock_guard lock(g_mutex);
    *left = g_rumble[0] / 65535.0f;
    *right = g_rumble[1] / 65535.0f;
}

}  // namespace vvr
