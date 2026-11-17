#include "vr.h"

#include <windows.h>
#include <dxgi1_4.h>
#include <wrl/client.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <string>

#include "config.h"
#include "frame.h"
#include "gamepad.h"
#include "hooks.h"
#include "log.h"
#include "mathx.h"
#include "render.h"
#include "stereo_device.h"
#include "ue5.h"
#include "weapon.h"
#include "xr.h"

using Microsoft::WRL::ComPtr;
using namespace vvr::mathx;

namespace vvr {

namespace {

// FViewport::Draw(bool bShouldPresent): one frame of a viewport (game thread).
constexpr const char* VIEWPORT_DRAW_SIG =
    "40 55 56 41 55 41 57 48 8D AC 24 38 FF FF FF 48 81 EC C8 01 00 00 48 8B 05 ?? ?? ?? ?? "
    "48 33 C4 48 89 85 A0 00 00 00 48 8B F1 88 54 24 40 48 8B 49 30";
constexpr uint32_t kViewportClient = 0x30;  // FViewport::ViewportClient
// UGameViewportClient's FViewportClient subobject (its vtable thunks adjust by 0x28).
constexpr uint32_t kGameViewportClientInterface = 0x28;
using ViewportDrawFn = void (*)(void* viewport, bool should_present);
ViewportDrawFn g_viewport_draw = nullptr;

bool g_engine_ok = false;
std::atomic<bool> g_xr_ok{false};

// ---- command line ----
using GetCommandLineWFn = LPWSTR(WINAPI*)();
GetCommandLineWFn g_get_command_line = nullptr;
std::wstring g_command_line;

LPWSTR WINAPI hook_get_command_line() {
    return g_command_line.data();
}

// ---- head tracking reference (OpenXR LOCAL space) ----
bool g_recenter_pending = true;
double g_ref_yaw = 0.0;
Vec3 g_ref_position{};

// ---- player objects ----
ue::UObject* g_pc = nullptr;  // local PlayerController (validated every frame)

bool key_pressed(int vk, bool& was_down) {
    const bool down = vk != 0 && (GetAsyncKeyState(vk) & 0x8000) != 0;
    const bool pressed = down && !was_down;
    was_down = down;
    return pressed;
}

bool game_has_focus() {
    DWORD pid = 0;
    GetWindowThreadProcessId(GetForegroundWindow(), &pid);
    return pid == GetCurrentProcessId();
}

XrQuaternionf yaw_pitch_quat(double yaw_degrees, double pitch_degrees) {
    return quat_mul(quat_yaw(yaw_degrees * kDegToRad), quat_pitch(pitch_degrees * kDegToRad));
}

// Quest 3-like optics, used without a headset (FakeHeadset=1).
void fake_views(XrView views[2], xr::ControllerInput& input) {
    const XrQuaternionf q = yaw_pitch_quat(config().fake_yaw, config().fake_pitch);
    for (int eye = 0; eye < 2; ++eye) {
        XrView& v = views[eye];
        v.type = XR_TYPE_VIEW;
        v.pose.orientation = eye == 1 ? quat_mul(quat_yaw(-config().fake_eye_split * kDegToRad), q) : q;
        v.pose.position = to_xr(rotate(q, {eye == 0 ? -0.032 : 0.032, 0.0, 0.0}));
        const float outer = 0.9425f, inner = 0.6981f;
        v.fov = {eye == 0 ? -outer : -inner, eye == 0 ? inner : outer, 0.7679f, -0.8727f};
    }
    input = xr::ControllerInput{};
    input.active = true;
    xr::HandInput& right = input.hand[xr::kHandRight];
    right.pose_valid = true;
    right.aim.orientation = yaw_pitch_quat(config().fake_hand_yaw, config().fake_hand_pitch);
    right.aim.position = {0.20f, -0.35f, -0.35f};
    if (config().fake_fire) {
        right.trigger = (GetTickCount() / 400) % 2 != 0 ? 1.0f : 0.0f;
    }
    xr::HandInput& left = input.hand[xr::kHandLeft];
    left.pose_valid = true;
    left.aim.orientation = yaw_pitch_quat(-25.0, 45.0);
    left.aim.position = {-0.10f, -0.17f, -0.38f};
}

void recenter(const XrView views[2]) {
    g_ref_yaw = xr_yaw(views[0].pose.orientation);
    g_ref_position = (to_vec(views[0].pose.position) + to_vec(views[1].pose.position)) * 0.5;
    if (config().fake_xr) {
        g_ref_yaw = 0.0;
        g_ref_position = {};
    }
    LOG("Recentred: yaw %.1f deg, head at (%.2f, %.2f, %.2f)", g_ref_yaw * kRadToDeg,
        g_ref_position.x, g_ref_position.y, g_ref_position.z);
}

// Eye poses relative to the recentred reference, in Unreal axes.
EyeFrame eye_frame(const XrView views[2]) {
    EyeFrame f;
    f.valid = true;
    const Vec3 middle = (to_vec(views[0].pose.position) + to_vec(views[1].pose.position)) * 0.5;
    for (int eye = 0; eye < 2; ++eye) {
        const XrView& v = views[eye];
        const Vec3 offset = config().positional ? to_vec(v.pose.position) - g_ref_position
                                                : to_vec(v.pose.position) - middle;
        f.offset[eye] = xr_to_ue(yaw_xr(offset, -g_ref_yaw));
        const XrQuaternionf& q = v.pose.orientation;
        f.forward[eye] = xr_to_ue(yaw_xr(rotate(q, {0, 0, -1}), -g_ref_yaw));
        f.right[eye] = xr_to_ue(yaw_xr(rotate(q, {1, 0, 0}), -g_ref_yaw));
        f.up[eye] = xr_to_ue(yaw_xr(rotate(q, {0, 1, 0}), -g_ref_yaw));
        f.tan_left[eye] = std::tan(v.fov.angleLeft);
        f.tan_right[eye] = std::tan(v.fov.angleRight);
        f.tan_up[eye] = std::tan(v.fov.angleUp);
        f.tan_down[eye] = std::tan(v.fov.angleDown);
    }
    return f;
}

XrPosef screen_pose(double distance) {
    // Quad `distance` metres in front of the recentred head, facing it.
    const Vec3 forward = yaw_xr({0.0, 0.0, -distance}, g_ref_yaw);
    XrPosef pose{};
    pose.position = to_xr({g_ref_position.x + forward.x, g_ref_position.y, g_ref_position.z + forward.z});
    pose.orientation = quat_yaw(g_ref_yaw);
    return pose;
}

// GEngine.GameInstance.LocalPlayers[0]
ue::UObject* local_player() {
    static int32_t off_instance = -1, off_players = -1;
    ue::UObject* engine = ue::engine();
    if (engine == nullptr || !ue::is_live(engine)) {
        return nullptr;
    }
    if (off_instance < 0) {
        off_instance = ue::property_offset(ue::class_of(engine), "GameInstance");
    }
    ue::UObject* instance = off_instance >= 0 ? ue::field<ue::UObject*>(engine, off_instance) : nullptr;
    if (!ue::is_live(instance)) {
        return nullptr;
    }
    if (off_players < 0) {
        off_players = ue::property_offset(ue::class_of(instance), "LocalPlayers");
        LOG("Offsets: GameInstance=0x%X LocalPlayers=0x%X", off_instance, off_players);
    }
    if (off_players < 0) {
        return nullptr;
    }
    const auto& players = ue::field<ue::TArray<ue::UObject*>>(instance, off_players);
    return players.count > 0 && ue::is_live(players.data[0]) ? players.data[0] : nullptr;
}

// LocalPlayers[0].PlayerController
ue::UObject* resolve_player() {
    static int32_t off_controller = -1;
    ue::UObject* player = local_player();
    if (player == nullptr) {
        return nullptr;
    }
    if (off_controller < 0) {
        off_controller = ue::property_offset(ue::class_of(player), "PlayerController");
        LOG("Offset: PlayerController=0x%X", off_controller);
    }
    ue::UObject* pc = off_controller >= 0 ? ue::field<ue::UObject*>(player, off_controller) : nullptr;
    return ue::is_live(pc) ? pc : nullptr;
}

// ULocalPlayer::ViewStates: TArray<FSceneViewStateReference> indexed by stereo view index,
// 0x38-byte elements holding the FSceneViewStateInterface* at +8. The offset is read from
// ULocalPlayer::CalcSceneViewInitOptions ("lea rcx,[rdi+num]; lea r14,[rdi+data]").
constexpr const char* VIEW_STATES_SIG =
    "48 8D 8F ?? ?? ?? ?? 4C 8D B7 ?? ?? ?? ?? 0F 45 EB 44 8B E5 44 2B 21";
constexpr size_t kViewStateRefSize = 0x38;
constexpr size_t kViewStateRefPointer = 0x08;
int32_t g_view_states_offset = -1;

// The view state (temporal history, Lumen and shadow caches) of each stereo view index.
void** view_state_slot(ue::UObject* player, int index) {
    if (player == nullptr || g_view_states_offset < 0) {
        return nullptr;
    }
    const auto& states = ue::field<ue::TArray<uint8_t>>(player, g_view_states_offset);
    if (states.data == nullptr || index >= states.count) {
        return nullptr;
    }
    return reinterpret_cast<void**>(states.data + index * kViewStateRefSize + kViewStateRefPointer);
}

// ---- debug triggers (Trace=1): files in Binaries\Win64 ----
void dump_objects(const char* query) {
    // "fn:Text" lists functions whose path contains Text, "path:Text" objects whose path
    // contains Text; anything else is a class name (live instances and their properties).
    const bool functions = strncmp(query, "fn:", 3) == 0;
    const bool by_path = strncmp(query, "path:", 5) == 0;
    int listed = 0;
    for (int32_t i = 0; i < ue::object_count() && listed < 400; ++i) {
        ue::UObject* obj = ue::object_at(i);
        if (obj == nullptr || !ue::is_live(obj) || ue::class_of(obj) == nullptr) {
            continue;
        }
        bool match = false;
        if (functions) {
            match = ue::is_a(obj, "Function") && ue::full_name(obj).find(query + 3) != std::string::npos;
        } else if (by_path) {
            match = ue::full_name(obj).find(query + 5) != std::string::npos;
        } else {
            match = ue::is_a(obj, query);
        }
        if (!match) {
            continue;
        }
        ++listed;
        LOG("OBJ %p %s%s", (void*)obj, ue::full_name(obj).c_str(),
            ue::is_default_object(obj) ? " (default)" : "");
        if (functions) {
            for (ue::FProperty* p = ue::first_property(obj); p != nullptr; p = ue::next_property(p)) {
                LOG("    %s %s @0x%X", ue::property_class_name(p).c_str(), ue::property_name(p).c_str(),
                    ue::field<int32_t>(p, ue::off::property_offset));
            }
            continue;
        }
        if (by_path || ue::is_default_object(obj)) {
            continue;
        }
        for (const ue::UStruct* s = ue::class_of(obj); s != nullptr; s = ue::super_of(s)) {
            for (ue::FProperty* p = ue::first_property(s); p != nullptr; p = ue::next_property(p)) {
                const std::string kind = ue::property_class_name(p);
                const std::string name = ue::property_name(p);
                const int32_t offset = ue::field<int32_t>(p, ue::off::property_offset);
                if (kind == "BoolProperty") {
                    const auto b = ue::find_bool(s, name.c_str());
                    LOG("    bool %s = %d @0x%X", name.c_str(), b.get(obj), offset);
                } else if (kind == "ObjectProperty" || kind == "WeakObjectProperty") {
                    ue::UObject* target = kind == "ObjectProperty" ? ue::field<ue::UObject*>(obj, offset) : nullptr;
                    LOG("    obj %s -> %s @0x%X", name.c_str(),
                        target ? ue::full_name(target).c_str() : "None", offset);
                } else if (kind == "FloatProperty") {
                    LOG("    float %s = %g @0x%X", name.c_str(), ue::field<float>(obj, offset), offset);
                } else if (kind == "DoubleProperty") {
                    LOG("    double %s = %g @0x%X", name.c_str(), ue::field<double>(obj, offset), offset);
                } else if (kind == "IntProperty" || kind == "ByteProperty" || kind == "EnumProperty") {
                    LOG("    %s %s = %d @0x%X", kind.c_str(), name.c_str(),
                        kind == "IntProperty" ? ue::field<int32_t>(obj, offset) : ue::field<uint8_t>(obj, offset),
                        offset);
                } else if (kind == "StructProperty") {
                    const auto& v = ue::field<ue::FVector>(obj, offset);
                    LOG("    struct %s [%d] = (%g, %g, %g) @0x%X", name.c_str(),
                        ue::field<int32_t>(p, ue::off::property_element_size), v.x, v.y, v.z, offset);
                } else if (kind == "ArrayProperty") {
                    LOG("    array %s count=%d @0x%X", name.c_str(),
                        ue::field<ue::TArray<void*>>(obj, offset).count, offset);
                } else {
                    LOG("    %s %s @0x%X", kind.c_str(), name.c_str(), offset);
                }
            }
        }
    }
    LOG("%d objects listed for '%s'", listed, query);
}

void handle_debug_triggers() {
    if (!config().trace) {
        return;
    }
    static DWORD last = 0;
    const DWORD now = GetTickCount();
    if (now - last < 500) {
        return;
    }
    last = now;
    const std::wstring dump = config().dir + L"voidvr_dump.txt";
    if (GetFileAttributesW(dump.c_str()) != INVALID_FILE_ATTRIBUTES) {
        DeleteFileW(dump.c_str());
        render_request_dump();
    }
    const std::wstring objs = config().dir + L"voidvr_objs.txt";
    if (FILE* f = _wfopen(objs.c_str(), L"r")) {
        char query[256] = {};
        fgets(query, sizeof(query), f);
        fclose(f);
        DeleteFileW(objs.c_str());
        query[strcspn(query, "\r\n")] = 0;
        dump_objects(query);
    }
    // voidvr_call.txt: "ClassName FunctionName" calls the function (zeroed parameters) on the
    // first live instance of that class.
    const std::wstring call = config().dir + L"voidvr_call.txt";
    if (FILE* f = _wfopen(call.c_str(), L"r")) {
        char cls[256] = {}, fn[256] = {};
        const int n = fscanf_s(f, "%255s %255s", cls, 256u, fn, 256u);
        fclose(f);
        DeleteFileW(call.c_str());
        ue::UObject* obj = n == 2 ? ue::find_first_instance(cls) : nullptr;
        ue::UFunction* func = obj ? ue::find_function(ue::class_of(obj), fn) : nullptr;
        if (func != nullptr) {
            std::vector<uint8_t> params(ue::params_size(func) + 16, 0);
            LOG("Calling %s on %s", fn, ue::full_name(obj).c_str());
            ue::call_function(obj, func, params.data());
        } else {
            LOG("Call failed: %s %s (object %p)", cls, fn, (void*)obj);
        }
    }
    const std::wstring pad = config().dir + L"voidvr_pad.txt";
    if (FILE* f = _wfopen(pad.c_str(), L"r")) {
        char line[256] = {};
        fgets(line, sizeof(line), f);
        fclose(f);
        DeleteFileW(pad.c_str());
        gamepad_debug_press(line);
    }
    const std::wstring cmd = config().dir + L"voidvr_cmd.txt";
    if (FILE* f = _wfopen(cmd.c_str(), L"r")) {
        wchar_t line[512];
        while (fgetws(line, 512, f) != nullptr) {
            std::wstring text = line;
            while (!text.empty() && (text.back() == L'\n' || text.back() == L'\r')) {
                text.pop_back();
            }
            if (!text.empty() && g_pc != nullptr) {
                ue::console_command(g_pc, text.c_str());
            }
        }
        fclose(f);
        DeleteFileW(cmd.c_str());
    }
}

// The game viewport: GEngine->GameViewport's FViewportClient interface draws into it.
bool is_game_viewport(void* viewport) {
    static int32_t off_game_viewport = -1;
    ue::UObject* engine = ue::engine();
    if (engine == nullptr) {
        return false;
    }
    if (off_game_viewport < 0) {
        off_game_viewport = ue::property_offset(ue::class_of(engine), "GameViewport");
        if (off_game_viewport < 0) {
            return false;
        }
    }
    auto* client = ue::field<uint8_t*>(engine, off_game_viewport);
    return client != nullptr &&
           ue::field<void*>(viewport, kViewportClient) == client + kGameViewportClientInterface;
}

// A menu is open when the game shows its cursor or is paused: the UI then goes on the
// virtual screen and the controllers drive it.
bool game_in_menu() {
    if (g_pc == nullptr) {
        return true;
    }
    static ue::BoolRef cursor;
    static bool cursor_done = false;
    if (!cursor_done) {
        cursor_done = true;
        cursor = ue::find_bool(ue::class_of(g_pc), "bShowMouseCursor");
        LOG("PlayerController.bShowMouseCursor at +0x%X mask 0x%X", cursor.offset, cursor.mask);
    }
    if (cursor.get(g_pc)) {
        return true;
    }
    static ue::UObject* statics = nullptr;
    static ue::UFunction* is_paused = nullptr;
    static int32_t ctx_off = -1, ret_off = -1;
    if (is_paused == nullptr) {
        statics = ue::find_default_object("GameplayStatics");
        is_paused = statics ? ue::find_function(ue::class_of(statics), "IsGamePaused") : nullptr;
        if (is_paused == nullptr) {
            return false;
        }
        ctx_off = ue::property_offset(is_paused, "WorldContextObject");
        ret_off = ue::property_offset(is_paused, "ReturnValue");
    }
    if (ctx_off < 0 || ret_off < 0) {
        return false;
    }
    uint8_t params[64] = {};
    ue::field<ue::UObject*>(params, ctx_off) = g_pc;
    ue::call_function(statics, is_paused, params);
    return params[ret_off] != 0;
}

// Rotation about an axis (unit) by `radians`.
XrQuaternionf quat_axis(Vec3 axis, double radians) {
    const double s = std::sin(radians * 0.5);
    return {static_cast<float>(axis.x * s), static_cast<float>(axis.y * s),
            static_cast<float>(axis.z * s), static_cast<float>(std::cos(radians * 0.5))};
}

// The HUD blocks of the game's UI image (fractions of its size, 16:9) shown on the left wrist
// like a watch: vitals/resources (bottom left) and weapon/ammo (bottom right). The rest of the
// HUD (crosshair included) stays out of view in gameplay.
struct HudBlock {
    float x0, y0, x1, y1;  // UI image fractions
    float offset;          // along the panel's width, metres from the wrist centre
};
constexpr HudBlock kWristBlocks[] = {
    {0.012f, 0.600f, 0.103f, 0.834f, -0.045f},  // resources (keys, scrap, voidium)
    {0.880f, 0.790f, 0.985f, 0.960f, 0.050f},   // weapon and ammo
};
constexpr float kWristBlockHeight = 0.075f;  // metres

void place_wrist_hud(FrameSlot& slot, const xr::ControllerInput& input) {
    const xr::HandInput& hand = input.hand[xr::kHandLeft];
    uint32_t ui_w = 0, ui_h = 0;
    render_backbuffer_size(&ui_w, &ui_h);
    if (!input.active || !hand.pose_valid || ui_w == 0 || ui_h == 0) {
        return;
    }
    // On the back of the left wrist (aim space: +Y up, +Z towards the wrist), facing up and
    // turned about the forearm towards the face; text up points to the fingers.
    const XrQuaternionf& q = hand.aim.orientation;
    const double tilt = 20.0 * kDegToRad;
    const XrQuaternionf local = quat_mul(quat_axis({0, 0, 1}, -tilt), quat_axis({1, 0, 0}, -kPi / 2));
    const XrQuaternionf orientation = quat_mul(q, local);
    const Vec3 centre = to_vec(hand.aim.position) + rotate(q, {0.0, 0.03, 0.12});
    for (const HudBlock& b : kWristBlocks) {
        xr::Quad quad;
        const int32_t x0 = static_cast<int32_t>(b.x0 * ui_w), y0 = static_cast<int32_t>(b.y0 * ui_h);
        const int32_t x1 = static_cast<int32_t>(b.x1 * ui_w), y1 = static_cast<int32_t>(b.y1 * ui_h);
        quad.rect = {{x0, y0}, {x1 - x0, y1 - y0}};
        quad.size = {kWristBlockHeight * (x1 - x0) / static_cast<float>(y1 - y0), kWristBlockHeight};
        quad.pose.orientation = orientation;
        quad.pose.position = to_xr(centre + rotate(orientation, {b.offset, 0.0, 0.0}));
        slot.hud.push_back(quad);
    }
}

// HUD pieces that stay in view (fixed to the head, low): health, subtitles/dialogue and the
// game's notices. Rects are UI image fractions; positions in metres in the head's frame
// (x right, y up, z forward), width in metres.
struct ViewPanel {
    float x0, y0, x1, y1;
    float x, y, z, width;
};
constexpr ViewPanel kViewPanels[] = {
    // Abilities and health bar, in two pieces that tile: the HUD is slanted, so the left piece
    // starts lower to leave out the bottom of the resource boxes above it.
    {0.025f, 0.841f, 0.105f, 0.945f, -0.563f, -0.3824f, 1.5f, 0.1344f},
    {0.105f, 0.826f, 0.275f, 0.945f, -0.353f, -0.3752f, 1.5f, 0.2856f},
    {0.285f, 0.785f, 0.750f, 0.890f, 0.03f, -0.50f, 1.5f, 0.75f},   // subtitles and dialogue
    {0.280f, 0.630f, 0.720f, 0.730f, 0.00f, -0.28f, 1.5f, 0.70f},   // notices (low ammo...)
};

void place_view_panels(FrameSlot& slot) {
    uint32_t ui_w = 0, ui_h = 0;
    render_backbuffer_size(&ui_w, &ui_h);
    if (!slot.stereo || ui_w == 0 || ui_h == 0) {
        return;
    }
    const XrQuaternionf& q = slot.views[0].pose.orientation;
    const Vec3 head = (to_vec(slot.views[0].pose.position) + to_vec(slot.views[1].pose.position)) * 0.5;
    for (const ViewPanel& p : kViewPanels) {
        xr::Quad quad;
        const int32_t x0 = static_cast<int32_t>(p.x0 * ui_w), y0 = static_cast<int32_t>(p.y0 * ui_h);
        const int32_t x1 = static_cast<int32_t>(p.x1 * ui_w), y1 = static_cast<int32_t>(p.y1 * ui_h);
        quad.rect = {{x0, y0}, {x1 - x0, y1 - y0}};
        quad.size = {p.width, p.width * (y1 - y0) / static_cast<float>(x1 - x0)};
        quad.pose.orientation = q;
        quad.pose.position = to_xr(head + rotate(q, {p.x, p.y, -p.z}));
        slot.hud.push_back(quad);
    }
}

// ---- body, turning and aiming (AimMode 2) ----
// The body's yaw (degrees, Unreal world): the eyes are placed around it and the right stick
// turns it, while the game's control rotation follows the aiming hand.
double g_body_yaw = 0.0;
bool g_body_valid = false;
ue::UObject* g_body_pc = nullptr;

bool controller_aims() {
    return config().aim_mode == 2;
}

void update_body_yaw(const xr::ControllerInput& input, bool in_menu) {
    static ULONGLONG last_ms = GetTickCount64();
    const ULONGLONG now_ms = GetTickCount64();
    const double dt = std::min(0.1, (now_ms - last_ms) / 1000.0);
    last_ms = now_ms;
    if (g_pc != g_body_pc) {
        g_body_pc = g_pc;
        g_body_valid = false;
    }
    if (!g_body_valid && g_pc != nullptr) {
        ue::Call get(g_pc, "GetControlRotation");
        if (get.run()) {
            g_body_yaw = get.get<ue::FRotator>("ReturnValue").yaw;
            g_body_valid = true;
            LOG("Body yaw from the game: %.1f", g_body_yaw);
        }
    }
    if (in_menu || !input.active) {
        return;
    }
    const float x = input.hand[xr::kHandRight].stick.x;
    if (config().snap_turn > 0.0f) {
        static bool armed = true;
        if (armed && std::fabs(x) > 0.7f) {
            armed = false;
            g_body_yaw += x > 0 ? config().snap_turn : -config().snap_turn;
        } else if (std::fabs(x) < 0.3f) {
            armed = true;
        }
    } else if (std::fabs(x) > 0.15f) {
        const float amount = (std::fabs(x) - 0.15f) / 0.85f * (x > 0 ? 1.0f : -1.0f);
        g_body_yaw += amount * config().turn_speed * dt;
    }
    g_body_yaw = wrap_degrees(g_body_yaw);
}

// Head yaw in the Unreal world (degrees): the body's yaw plus the head's turn from the
// recentred reference (OpenXR yaw is positive to the left, Unreal's to the right).
double head_world_yaw(const FrameSlot& slot) {
    if (!slot.stereo) {
        return g_body_yaw;
    }
    return g_body_yaw - (xr_yaw(slot.views[0].pose.orientation) - g_ref_yaw) * kRadToDeg;
}

// A square crop of the UI image shown at a world point (Unreal units), facing the head, with
// `angular` metres of size per metre of distance. `camera` is the reference head position.
void place_world_icon(FrameSlot& slot, Vec3 world, Vec3 camera, XrRect2Di rect, double angular,
                      bool marker = false) {
    // World -> LOCAL space: the inverse of the eye placement (see eye_frame / stereo_device).
    const double scale = stereo_world_scale();
    const Vec3 rel = yaw_ue(world - camera, -g_body_yaw * kDegToRad) * (1.0 / scale);
    Vec3 local = yaw_xr({rel.y, rel.z, -rel.x}, g_ref_yaw) + g_ref_position;
    const Vec3 head = (to_vec(slot.views[0].pose.position) + to_vec(slot.views[1].pose.position)) * 0.5;
    Vec3 to_head = head - local;
    double distance = length(to_head);
    if (distance < 0.05) {
        return;
    }
    to_head = to_head * (1.0 / distance);
    // Far points are drawn nearer (same direction and apparent size): runtimes clip far quads.
    constexpr double kMaxDistance = 60.0;
    if (distance > kMaxDistance) {
        local = head - to_head * kMaxDistance;
        distance = kMaxDistance;
    }
    const double yaw = std::atan2(to_head.x, to_head.z);
    const double pitch = std::asin(std::clamp(to_head.y, -1.0, 1.0));
    xr::Quad quad;
    quad.pose.orientation = quat_mul(quat_yaw(yaw), quat_pitch(-pitch));
    quad.pose.position = to_xr(local);
    const float size = static_cast<float>(distance * angular);
    quad.size = {size, size * rect.extent.height / static_cast<float>(rect.extent.width)};
    quad.rect = rect;
    quad.marker = marker;
    slot.hud.push_back(quad);
}

// The game's own crosshair (centre of its UI image) drawn where the gun's line of fire hits.
void place_reticle(FrameSlot& slot) {
    Vec3 hit, camera;
    uint32_t ui_w = 0, ui_h = 0;
    render_backbuffer_size(&ui_w, &ui_h);
    if (ui_w == 0 || !weapon_trace(50000.0, &hit, &camera)) {
        return;
    }
    const int32_t px = static_cast<int32_t>(ui_h * 0.07);  // crosshair crop, square
    place_world_icon(slot, hit, camera,
                     {{static_cast<int32_t>(ui_w / 2) - px / 2, static_cast<int32_t>(ui_h / 2) - px / 2}, {px, px}},
                     0.14);
}

// The game's world indicators (objective, chests, obelisks...) as waypoint pins in 3D: each
// actor whose screen indicator is showing (its widget exists) gets the mod's pin above it.
std::vector<ue::UObject*> g_indicators;  // ScreenIndicator components

void refresh_indicators() {
    static ULONGLONG last = 0;
    const ULONGLONG now = GetTickCount64();
    if (now - last < 1000) {
        return;
    }
    last = now;
    g_indicators.clear();
    // The component class, from an actor that declares one (the widget class has the same name).
    static ue::UClass* cls = nullptr;
    if (cls == nullptr) {
        const ue::UClass* marker = ue::find_class("BP_MissionObjective_Marker_C");
        ue::FProperty* prop = marker ? ue::find_property(marker, "ScreenIndicator") : nullptr;
        cls = prop ? ue::field<ue::UClass*>(prop, ue::off::object_property_class) : nullptr;
        if (cls == nullptr) {
            return;
        }
    }
    for (int32_t i = 0; i < ue::object_count(); ++i) {
        ue::UObject* obj = ue::object_at(i);
        if (obj != nullptr && ue::class_of(obj) == cls && !ue::is_default_object(obj)) {
            g_indicators.push_back(obj);
        }
    }
}

void place_world_indicators(FrameSlot& slot, int budget) {
    Vec3 camera;
    if (budget <= 0 || !weapon_camera(&camera)) {
        return;
    }
    refresh_indicators();
    static int32_t off_widget = -1;
    struct Shown {
        double distance;
        ue::FVector where;
        XrRect2Di rect;
    };
    std::vector<Shown> shown;
    for (ue::UObject* indicator : g_indicators) {
        if (!ue::is_live(indicator)) {
            continue;
        }
        if (off_widget < 0) {
            off_widget = ue::property_offset(ue::class_of(indicator), "Widget");
            if (off_widget < 0) {
                return;
            }
        }
        ue::UObject* actor = ue::outer_of(indicator);
        if (ue::field<ue::UObject*>(indicator, off_widget) == nullptr || actor == nullptr || !ue::is_live(actor)) {
            continue;  // not shown by the game
        }
        ue::Call where(actor, "K2_GetActorLocation");
        if (!where.run()) {
            continue;
        }
        const ue::FVector p = where.get<ue::FVector>("ReturnValue");
        const Vec3 d = Vec3{p.x, p.y, p.z} - camera;
        // Nearby things only (chests, obelisks...); mission objectives at any distance.
        static const ue::UClass* objective = ue::find_class("BP_MissionObjective_Marker_C");
        constexpr double kMaxPinDistance = 2500.0;  // 25 m
        if (length(d) > kMaxPinDistance && ue::class_of(actor) != objective) {
            continue;
        }
        shown.push_back({length(d), p, {{0, 0}, {xr::kMarkerSize, xr::kMarkerSize}}});
    }
    // The nearest ones fit in the layer budget: a waypoint pin floating above each. Actors
    // close together or in nearly the same direction (stacked markers) share one pin.
    std::sort(shown.begin(), shown.end(), [](const Shown& a, const Shown& b) { return a.distance < b.distance; });
    std::vector<Vec3> placed;  // unit directions from the camera of the pins drawn
    std::vector<Vec3> placed_at;
    for (const Shown& s : shown) {
        if (static_cast<int>(placed.size()) >= budget) {
            break;
        }
        const Vec3 at{s.where.x, s.where.y, s.where.z + 120.0};
        const Vec3 dir = (at - camera) * (1.0 / std::max(1.0, length(at - camera)));
        bool duplicate = false;
        for (size_t i = 0; i < placed.size() && !duplicate; ++i) {
            duplicate = dot(placed[i], dir) > std::cos(5.0 * kDegToRad) || length(placed_at[i] - at) < 400.0;
        }
        if (duplicate) {
            continue;
        }
        placed.push_back(dir);
        placed_at.push_back(at);
        place_world_icon(slot, at, camera, s.rect, 0.035, true);
    }
}

// Object info panels (mod previews, obelisks...) are widget components the game draws flat
// on the screen. In VR they become floating panels in the world (holograms): world space,
// scaled to a readable size, lifted above their object and turned towards the player.
struct Hologram {
    ue::UObject* component;
    bool lift;  // an object's info panel: float it above the object
};
std::vector<Hologram> g_holograms;
constexpr double kPanelScale = 0.25;  // widget pixels -> Unreal units (cm), info panels
constexpr double kSmallScale = 0.15;  // enemy health bars, damage numbers
constexpr double kPanelLift = 50.0;   // cm between the object and the panel's bottom edge

void update_holograms() {
    static const ue::UClass* cls = ue::find_class("WidgetComponent");
    static int32_t off_space = -1, off_size = -1;
    if (cls == nullptr) {
        return;
    }
    static ULONGLONG last = 0;
    const ULONGLONG now = GetTickCount64();
    if (now - last >= 1000) {
        last = now;
        if (off_space < 0) {
            off_space = ue::property_offset(cls, "Space");
            off_size = ue::property_offset(cls, "CurrentDrawSize");
            if (off_space < 0 || off_size < 0) {
                return;
            }
        }
        for (int32_t i = 0; i < ue::object_count(); ++i) {
            ue::UObject* obj = ue::object_at(i);
            if (obj == nullptr || ue::class_of(obj) != cls || ue::is_default_object(obj)) {
                continue;
            }
            if (ue::field<uint8_t>(obj, off_space) != 1) {  // EWidgetSpace::Screen
                continue;
            }
            ue::UObject* owner = ue::outer_of(obj);
            const std::string owner_class = owner ? ue::object_name(ue::class_of(owner)) : "";
            const bool compact = owner_class.rfind("CH_", 0) == 0 || owner_class.find("DamageNumber") != std::string::npos;
            const double s = compact ? kSmallScale : kPanelScale;
            ue::Call to_world(obj, "SetWidgetSpace");
            to_world.set<uint8_t>("NewSpace", 0).run();  // EWidgetSpace::World
            ue::Call two_sided(obj, "SetTwoSided");
            two_sided.set<bool>("bWantTwoSided", true).run();
            ue::Call scale(obj, "SetWorldScale3D");
            scale.set("NewScale", ue::FVector{s, s, s}).run();
            ue::Call(obj, "RequestRenderUpdate").run();
            g_holograms.push_back({obj, !compact});
            LOG("Hologram: %s (%s)", ue::full_name(obj).c_str(), compact ? "small" : "panel");
        }
    }
    Vec3 camera;
    if (!weapon_camera(&camera)) {
        return;
    }
    std::vector<Hologram> live;
    for (const Hologram& h : g_holograms) {
        ue::UObject* obj = h.component;
        if (!ue::is_live(obj) || ue::class_of(obj) != cls) {
            continue;
        }
        live.push_back(h);
        ue::FVector p{};
        ue::UObject* owner = ue::outer_of(obj);
        if (h.lift && owner != nullptr && ue::is_live(owner)) {
            ue::Call where(owner, "K2_GetActorLocation");
            if (!where.run()) {
                continue;
            }
            p = where.get<ue::FVector>("ReturnValue");
            const int32_t height = ue::field<int32_t>(obj, off_size + 4);  // FIntPoint Y
            p.z += kPanelLift + 0.5 * kPanelScale * std::clamp(height, 0, 2000);
        } else {
            ue::Call where(obj, "K2_GetComponentLocation");
            if (!where.run()) {
                continue;
            }
            p = where.get<ue::FVector>("ReturnValue");
        }
        // Facing the player (the widget's front is the component's +X).
        const Vec3 to_cam = camera - Vec3{p.x, p.y, p.z};
        const double yaw = std::atan2(to_cam.y, to_cam.x) * kRadToDeg;
        const double pitch = std::atan2(to_cam.z, std::sqrt(to_cam.x * to_cam.x + to_cam.y * to_cam.y)) * kRadToDeg;
        ue::Call move(obj, "K2_SetWorldLocationAndRotation");
        move.set("NewLocation", p).set("NewRotation", ue::FRotator{pitch, yaw, 0.0}).set<bool>("bTeleport", true);
        move.run();
    }
    g_holograms.swap(live);
}

HandPose hand_pose(const xr::HandInput& hand) {
    HandPose p;
    if (!hand.pose_valid) {
        return p;
    }
    p.valid = true;
    const Vec3 position = to_vec(hand.aim.position) - g_ref_position;
    p.offset = xr_to_ue(yaw_xr(position, -g_ref_yaw));
    const XrQuaternionf& q = hand.aim.orientation;
    p.forward = xr_to_ue(yaw_xr(rotate(q, {0, 0, -1}), -g_ref_yaw));
    p.right = xr_to_ue(yaw_xr(rotate(q, {1, 0, 0}), -g_ref_yaw));
    p.up = xr_to_ue(yaw_xr(rotate(q, {0, 1, 0}), -g_ref_yaw));
    return p;
}

// Both eyes are drawn as stereo view 0, each in a frame of the viewport of its own (one of its
// two buffered targets, see stereo_device.cpp): this renderer only outputs view index 0 (view 1
// stays black in any target layout). The right eye keeps its own temporal history, Lumen and
// shadow caches by lending ViewStates[1] to view 0; that state is created by drawing the right
// eye once as view 1.
void draw_eyes(void* viewport, bool should_present) {
    stereo_set_draw_eye(0, 0);
    g_viewport_draw(viewport, should_present);
    ue::UObject* player = local_player();
    void** left = view_state_slot(player, 0);
    void** right = view_state_slot(player, 1);
    if (g_view_states_offset < 0) {
        stereo_set_draw_eye(1, 0);
        g_viewport_draw(viewport, should_present);
    } else if (left == nullptr || right == nullptr || *left == nullptr || *right == nullptr) {
        stereo_set_draw_eye(1, 1);
        g_viewport_draw(viewport, should_present);
    } else {
        std::swap(*left, *right);
        stereo_set_draw_eye(1, 0);
        g_viewport_draw(viewport, should_present);
        left = view_state_slot(player, 0);
        right = view_state_slot(player, 1);
        if (left != nullptr && right != nullptr) {
            std::swap(*left, *right);
        }
    }
    stereo_set_draw_eye(-1, -1);
}

// Per frame: headset and controller state, then the viewport drawn once per eye.
void viewport_draw_hook(void* viewport, bool should_present) {
    static bool recenter_down = false;
    if (!g_engine_ok || !stereo_enabled() || !is_game_viewport(viewport)) {
        g_viewport_draw(viewport, should_present);
        return;
    }
    g_pc = resolve_player();
    static bool device_logged = false;
    if (!device_logged && ue::engine() != nullptr) {
        device_logged = true;
        // UEngine::StereoRenderingDevice (TSharedPtr object at +0x1030, see InitializeHMDDevice).
        void* device = ue::field<void*>(ue::engine(), 0x1030);
        LOG("Engine stereo device %p, vtable exe+%llX", device,
            device ? static_cast<unsigned long long>(*reinterpret_cast<uintptr_t*>(device) - exe_base()) : 0ull);
    }
    handle_debug_triggers();
    if (key_pressed(config().recenter_key, recenter_down) && game_has_focus()) {
        g_recenter_pending = true;
    }

    FrameSlot slot;
    xr::ControllerInput input;
    if (config().fake_xr) {
        fake_views(slot.views, input);
        slot.stereo = true;
    } else if (g_xr_ok) {
        xr::poll_events();
        if (xr::consume_recenter_request()) {
            g_recenter_pending = true;
        }
        if (xr::session_running()) {
            const XrTime time = xr::predicted_time();
            slot.stereo = xr::locate_views(time, slot.views);
            xr::input_sync(time, input);
        }
    }
    const bool in_menu = game_in_menu();
    const bool aiming = controller_aims() && g_pc != nullptr;
    if (aiming) {
        update_body_yaw(input, in_menu);
    }
    EyeFrame eyes;
    if (slot.stereo) {
        if (g_recenter_pending) {
            g_recenter_pending = false;
            recenter(slot.views);
        }
        eyes = eye_frame(slot.views);
        eyes.body_yaw_valid = aiming && g_body_valid;
        eyes.body_yaw = g_body_yaw;
    }
    stereo_set_frame(eyes);
    slot.screen = in_menu;
    if (!in_menu) {
        place_wrist_hud(slot, input);
        place_view_panels(slot);
    }
    slot.screen_pose = screen_pose(config().screen_distance);
    slot.screen_width = config().screen_width;

    // Gameplay with controller aiming: the gun on the right hand, walking where the head looks.
    float move_rotation = 0.0f;
    if (aiming && g_body_valid && !in_menu && input.active) {
        weapon_before_draw(g_pc, hand_pose(input.hand[xr::kHandRight]), g_body_yaw, stereo_world_scale());
        ue::FVector origin, direction;
        const double aim_yaw = weapon_aim(&origin, &direction)
                                   ? std::atan2(direction.y, direction.x) * kRadToDeg
                                   : g_body_yaw;
        move_rotation = static_cast<float>(wrap_degrees(head_world_yaw(slot) - aim_yaw) * kDegToRad);
        update_holograms();
        if (config().reticle && slot.stereo) {
            place_reticle(slot);
            place_world_indicators(slot, 15 - static_cast<int>(slot.hud.size()));
        }
    } else if (!in_menu && slot.stereo) {
        // The game's camera yaw is the body: the head's turn from the reference.
        move_rotation = static_cast<float>(-(xr_yaw(slot.views[0].pose.orientation) - g_ref_yaw));
    }
    frame_queue_push(slot);
    gamepad_publish(input, move_rotation, in_menu, !aiming);
    if (input.active) {
        float rumble_left = 0.0f, rumble_right = 0.0f;
        gamepad_rumble(&rumble_left, &rumble_right);
        xr::haptic(xr::kHandLeft, rumble_left);
        xr::haptic(xr::kHandRight, rumble_right);
    }
    static bool last_menu = !in_menu;
    if (in_menu != last_menu) {
        last_menu = in_menu;
        LOG("Menu %s", in_menu ? "open" : "closed");
    }

    if (config().trace) {
        static DWORD last_status = 0;
        if (GetTickCount() - last_status > 5000) {
            last_status = GetTickCount();
            const XrVector3f& head = slot.views[0].pose.position;
            LOG("Status: stereo=%d pc=%p head (%.2f %.2f %.2f) input=%d", slot.stereo, (void*)g_pc,
                head.x, head.y, head.z, input.active);
        }
    }
    draw_eyes(viewport, should_present);
    weapon_after_draw();
}

}  // namespace

void vr_early_init() {
    config_load();
    log_init((config().dir + L"voidvr.log").c_str());
    LOG("VOID/BREAKER VR starting (enabled=%d fake=%d trace=%d)", config().vr_enabled,
        config().fake_xr, config().trace);
    if (!config().vr_enabled || !hooks_init()) {
        return;
    }
    // The engine's built-in stereo renderer (see stereo_device.h).
    std::wstring args = L" -emulatestereo";
    if (!config().extra_args.empty()) {
        args += L" " + config().extra_args;
    }
    g_command_line = std::wstring(GetCommandLineW()) + args;
    LOG("Command line: %ls", g_command_line.c_str());
    HMODULE kernelbase = GetModuleHandleW(L"kernelbase.dll");
    void* target = kernelbase ? reinterpret_cast<void*>(GetProcAddress(kernelbase, "GetCommandLineW")) : nullptr;
    hook_function(target, reinterpret_cast<void*>(&hook_get_command_line),
                  reinterpret_cast<void**>(&g_get_command_line), "GetCommandLineW");
    render_install();
}

void vr_engine_init() {
    LOG("Engine starting: installing hooks");
    if (config().d3d_debug) {
        render_enable_debug_layer();
    }
    // An 8-bit back buffer (the UI is cut out by its alpha) and legacy D3D12 barriers (the eye
    // copy relies on resource states). Both are read when the device and window are created.
    ue::set_int_cvar(L"r.DefaultBackBufferPixelFormat", 0, 0, 6);
    ue::set_int_cvar(L"r.D3D12.PreferredBarrierImplementation", 1, 0, 2);
    if (!config().fake_xr) {
        g_xr_ok = xr::init_instance();
        if (!g_xr_ok) {
            LOG("No OpenXR headset: the game runs flat. Start Virtual Desktop / Quest Link before the game.");
        }
    }
    uint32_t eye_w = 1600, eye_h = 1680;
    if (g_xr_ok) {
        eye_w = static_cast<uint32_t>(xr::recommended_width() * config().resolution_scale);
        eye_h = static_cast<uint32_t>(xr::recommended_height() * config().resolution_scale);
    }
    if (config().eye_width > 0 && config().eye_height > 0) {
        eye_w = config().eye_width;
        eye_h = config().eye_height;
    }
    stereo_set_eye_size(eye_w, eye_h);

    g_engine_ok = ue::init() && stereo_device_install();
    if (g_engine_ok) {
        g_engine_ok = hook_function(reinterpret_cast<void*>(find_pattern(VIEWPORT_DRAW_SIG)),
                                    reinterpret_cast<void*>(&viewport_draw_hook),
                                    reinterpret_cast<void**>(&g_viewport_draw), "FViewport::Draw");
    }
    if (g_engine_ok) {
        if (const uintptr_t match = find_pattern(VIEW_STATES_SIG)) {
            g_view_states_offset = *reinterpret_cast<const int32_t*>(match + 10);
            LOG("ULocalPlayer::ViewStates at +0x%X", g_view_states_offset);
        } else {
            LOG("WARNING: ULocalPlayer::ViewStates not found: both eyes share one temporal history");
        }
    }
    if (!g_engine_ok) {
        LOG("ERROR: engine hooks missing: VR disabled");
        return;
    }
    gamepad_install();
    stereo_set_enabled(g_xr_ok || config().fake_xr);
}

void vr_on_swapchain(ID3D12Device* device, ID3D12CommandQueue* queue) {
    if (!g_xr_ok) {
        return;
    }
    LUID wanted{};
    const LUID have = device->GetAdapterLuid();
    if (xr::adapter_luid(&wanted) &&
        (wanted.LowPart != have.LowPart || wanted.HighPart != have.HighPart)) {
        LOG("WARNING: the game renders on another GPU than the headset's (LUID %08lX%08lX vs %08lX%08lX)",
            have.HighPart, have.LowPart, wanted.HighPart, wanted.LowPart);
    }
    if (!xr::create_session(device, queue)) {
        LOG("ERROR: OpenXR session failed: VR disabled");
        g_xr_ok = false;
        stereo_set_enabled(false);
    }
}

bool vr_active() {
    return stereo_enabled();
}

}  // namespace vvr
