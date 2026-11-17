#include <cstring>
#include <utility>
#include <vector>

#include "log.h"
#include "xr.h"

namespace vvr::xr {

namespace {

XrInstance g_instance = XR_NULL_HANDLE;
XrSession g_session = XR_NULL_HANDLE;
XrSpace g_local = XR_NULL_HANDLE;
XrActionSet g_set = XR_NULL_HANDLE;
XrPath g_hand_path[2] = {XR_NULL_PATH, XR_NULL_PATH};
XrSpace g_aim_space[2] = {XR_NULL_HANDLE, XR_NULL_HANDLE};
float g_haptic_amplitude[2] = {0.0f, 0.0f};

struct Actions {
    XrAction aim = XR_NULL_HANDLE;
    XrAction trigger = XR_NULL_HANDLE;
    XrAction grip = XR_NULL_HANDLE;
    XrAction stick = XR_NULL_HANDLE;
    XrAction stick_click = XR_NULL_HANDLE;
    XrAction primary = XR_NULL_HANDLE;
    XrAction secondary = XR_NULL_HANDLE;
    XrAction menu = XR_NULL_HANDLE;
    XrAction haptic = XR_NULL_HANDLE;
} g_actions;

XrPath path(const char* text) {
    XrPath p = XR_NULL_PATH;
    xrStringToPath(g_instance, text, &p);
    return p;
}

bool create_action(XrAction* out, const char* name, const char* localized, XrActionType type,
                   bool per_hand) {
    XrActionCreateInfo info{XR_TYPE_ACTION_CREATE_INFO};
    info.actionType = type;
    strcpy_s(info.actionName, name);
    strcpy_s(info.localizedActionName, localized);
    if (per_hand) {
        info.countSubactionPaths = 2;
        info.subactionPaths = g_hand_path;
    }
    const XrResult r = xrCreateAction(g_set, &info, out);
    if (XR_FAILED(r)) {
        LOG("xrCreateAction %s failed: %d", name, r);
        return false;
    }
    return true;
}

void suggest(const char* profile, const std::vector<std::pair<XrAction, const char*>>& bindings) {
    std::vector<XrActionSuggestedBinding> list;
    for (const auto& [action, binding] : bindings) {
        list.push_back({action, path(binding)});
    }
    XrInteractionProfileSuggestedBinding suggested{XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING};
    suggested.interactionProfile = path(profile);
    suggested.countSuggestedBindings = static_cast<uint32_t>(list.size());
    suggested.suggestedBindings = list.data();
    const XrResult r = xrSuggestInteractionProfileBindings(g_instance, &suggested);
    LOG("Bindings for %s: %d", profile, r);
}

bool read_bool(XrAction action, XrPath hand) {
    XrActionStateGetInfo get{XR_TYPE_ACTION_STATE_GET_INFO};
    get.action = action;
    get.subactionPath = hand;
    XrActionStateBoolean state{XR_TYPE_ACTION_STATE_BOOLEAN};
    return XR_SUCCEEDED(xrGetActionStateBoolean(g_session, &get, &state)) && state.isActive &&
           state.currentState;
}

float read_float(XrAction action, XrPath hand) {
    XrActionStateGetInfo get{XR_TYPE_ACTION_STATE_GET_INFO};
    get.action = action;
    get.subactionPath = hand;
    XrActionStateFloat state{XR_TYPE_ACTION_STATE_FLOAT};
    return XR_SUCCEEDED(xrGetActionStateFloat(g_session, &get, &state)) && state.isActive
               ? state.currentState
               : 0.0f;
}

XrVector2f read_vector(XrAction action, XrPath hand) {
    XrActionStateGetInfo get{XR_TYPE_ACTION_STATE_GET_INFO};
    get.action = action;
    get.subactionPath = hand;
    XrActionStateVector2f state{XR_TYPE_ACTION_STATE_VECTOR2F};
    if (XR_SUCCEEDED(xrGetActionStateVector2f(g_session, &get, &state)) && state.isActive) {
        return state.currentState;
    }
    return {0.0f, 0.0f};
}

}  // namespace

bool input_init(XrInstance instance, XrSession session, XrSpace local) {
    g_instance = instance;
    g_session = session;
    g_local = local;
    g_hand_path[kHandLeft] = path("/user/hand/left");
    g_hand_path[kHandRight] = path("/user/hand/right");

    XrActionSetCreateInfo set_info{XR_TYPE_ACTION_SET_CREATE_INFO};
    strcpy_s(set_info.actionSetName, "gameplay");
    strcpy_s(set_info.localizedActionSetName, "Gameplay");
    if (XR_FAILED(xrCreateActionSet(g_instance, &set_info, &g_set))) {
        LOG("xrCreateActionSet failed");
        return false;
    }
    Actions& a = g_actions;
    if (!create_action(&a.aim, "aim", "Aim", XR_ACTION_TYPE_POSE_INPUT, true) ||
        !create_action(&a.trigger, "trigger", "Trigger", XR_ACTION_TYPE_FLOAT_INPUT, true) ||
        !create_action(&a.grip, "grip", "Grip", XR_ACTION_TYPE_FLOAT_INPUT, true) ||
        !create_action(&a.stick, "thumbstick", "Thumbstick", XR_ACTION_TYPE_VECTOR2F_INPUT, true) ||
        !create_action(&a.stick_click, "thumbstick_click", "Thumbstick click",
                       XR_ACTION_TYPE_BOOLEAN_INPUT, true) ||
        !create_action(&a.primary, "primary", "A / X", XR_ACTION_TYPE_BOOLEAN_INPUT, true) ||
        !create_action(&a.secondary, "secondary", "B / Y", XR_ACTION_TYPE_BOOLEAN_INPUT, true) ||
        !create_action(&a.menu, "menu", "Menu", XR_ACTION_TYPE_BOOLEAN_INPUT, true) ||
        !create_action(&a.haptic, "haptic", "Vibration", XR_ACTION_TYPE_VIBRATION_OUTPUT, true)) {
        return false;
    }

    suggest("/interaction_profiles/oculus/touch_controller",
            {{a.aim, "/user/hand/left/input/aim/pose"},
             {a.aim, "/user/hand/right/input/aim/pose"},
             {a.trigger, "/user/hand/left/input/trigger/value"},
             {a.trigger, "/user/hand/right/input/trigger/value"},
             {a.grip, "/user/hand/left/input/squeeze/value"},
             {a.grip, "/user/hand/right/input/squeeze/value"},
             {a.stick, "/user/hand/left/input/thumbstick"},
             {a.stick, "/user/hand/right/input/thumbstick"},
             {a.stick_click, "/user/hand/left/input/thumbstick/click"},
             {a.stick_click, "/user/hand/right/input/thumbstick/click"},
             {a.primary, "/user/hand/left/input/x/click"},
             {a.primary, "/user/hand/right/input/a/click"},
             {a.secondary, "/user/hand/left/input/y/click"},
             {a.secondary, "/user/hand/right/input/b/click"},
             {a.menu, "/user/hand/left/input/menu/click"},
             {a.haptic, "/user/hand/left/output/haptic"},
             {a.haptic, "/user/hand/right/output/haptic"}});
    suggest("/interaction_profiles/valve/index_controller",
            {{a.aim, "/user/hand/left/input/aim/pose"},
             {a.aim, "/user/hand/right/input/aim/pose"},
             {a.trigger, "/user/hand/left/input/trigger/value"},
             {a.trigger, "/user/hand/right/input/trigger/value"},
             {a.grip, "/user/hand/left/input/squeeze/value"},
             {a.grip, "/user/hand/right/input/squeeze/value"},
             {a.stick, "/user/hand/left/input/thumbstick"},
             {a.stick, "/user/hand/right/input/thumbstick"},
             {a.stick_click, "/user/hand/left/input/thumbstick/click"},
             {a.stick_click, "/user/hand/right/input/thumbstick/click"},
             {a.primary, "/user/hand/left/input/a/click"},
             {a.primary, "/user/hand/right/input/a/click"},
             {a.secondary, "/user/hand/left/input/b/click"},
             {a.secondary, "/user/hand/right/input/b/click"},
             {a.menu, "/user/hand/left/input/system/click"},
             {a.haptic, "/user/hand/left/output/haptic"},
             {a.haptic, "/user/hand/right/output/haptic"}});

    for (int hand = 0; hand < 2; ++hand) {
        XrActionSpaceCreateInfo space{XR_TYPE_ACTION_SPACE_CREATE_INFO};
        space.action = a.aim;
        space.subactionPath = g_hand_path[hand];
        space.poseInActionSpace.orientation.w = 1.0f;
        xrCreateActionSpace(g_session, &space, &g_aim_space[hand]);
    }
    XrSessionActionSetsAttachInfo attach{XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO};
    attach.countActionSets = 1;
    attach.actionSets = &g_set;
    const XrResult r = xrAttachSessionActionSets(g_session, &attach);
    LOG("Controller actions attached: %d", r);
    return XR_SUCCEEDED(r);
}

void input_sync(XrTime time, ControllerInput& out) {
    out = ControllerInput{};
    if (g_set == XR_NULL_HANDLE) {
        return;
    }
    XrActiveActionSet active{g_set, XR_NULL_PATH};
    XrActionsSyncInfo sync{XR_TYPE_ACTIONS_SYNC_INFO};
    sync.countActiveActionSets = 1;
    sync.activeActionSets = &active;
    // XR_SESSION_NOT_FOCUSED (e.g. Quest menu open) is a success code: report no input.
    if (xrSyncActions(g_session, &sync) != XR_SUCCESS) {
        return;
    }
    out.active = true;
    for (int hand = 0; hand < 2; ++hand) {
        HandInput& h = out.hand[hand];
        const XrPath p = g_hand_path[hand];
        h.trigger = read_float(g_actions.trigger, p);
        h.grip = read_float(g_actions.grip, p);
        h.stick = read_vector(g_actions.stick, p);
        h.stick_click = read_bool(g_actions.stick_click, p);
        h.primary = read_bool(g_actions.primary, p);
        h.secondary = read_bool(g_actions.secondary, p);
        h.menu = read_bool(g_actions.menu, p);
        XrSpaceLocation location{XR_TYPE_SPACE_LOCATION};
        if (g_aim_space[hand] != XR_NULL_HANDLE &&
            XR_SUCCEEDED(xrLocateSpace(g_aim_space[hand], g_local, time, &location))) {
            const XrSpaceLocationFlags needed =
                XR_SPACE_LOCATION_ORIENTATION_VALID_BIT | XR_SPACE_LOCATION_POSITION_VALID_BIT;
            h.pose_valid = (location.locationFlags & needed) == needed;
            h.aim = location.pose;
        }
    }
}

void haptic(Hand hand, float amplitude) {
    if (g_set == XR_NULL_HANDLE) {
        return;
    }
    const float previous = g_haptic_amplitude[hand];
    g_haptic_amplitude[hand] = amplitude;
    XrHapticActionInfo info{XR_TYPE_HAPTIC_ACTION_INFO};
    info.action = g_actions.haptic;
    info.subactionPath = g_hand_path[hand];
    if (amplitude <= 0.0f) {
        if (previous > 0.0f) {
            xrStopHapticFeedback(g_session, &info);
        }
        return;
    }
    // Re-applied every frame while the game rumbles, so a short pulse never gets stuck on.
    XrHapticVibration vibration{XR_TYPE_HAPTIC_VIBRATION};
    vibration.amplitude = amplitude;
    vibration.duration = 100000000;  // 100 ms
    vibration.frequency = XR_FREQUENCY_UNSPECIFIED;
    xrApplyHapticFeedback(g_session, &info,
                          reinterpret_cast<const XrHapticBaseHeader*>(&vibration));
}

}  // namespace vvr::xr
