#include "weapon.h"

#include <array>
#include <cmath>
#include <cstring>

#include "config.h"
#include "log.h"

namespace vvr {

using namespace mathx;

namespace {

ue::UObject* g_arms = nullptr;  // arms moved for the current draw
ue::FVector g_saved_location{};
ue::FRotator g_saved_rotation{};
bool g_aim_valid = false;
ue::FVector g_aim_origin{}, g_aim_direction{};
ue::UObject* g_pawn = nullptr;
Vec3 g_camera{};  // camera location this frame (the reference head position)

ue::FVector to_fvector(Vec3 v) {
    return {v.x, v.y, v.z};
}

Vec3 to_vec3(const ue::FVector& v) {
    return {v.x, v.y, v.z};
}

Rotator to_rot(const ue::FRotator& r) {
    Rotator out;
    out.pitch = r.pitch;
    out.yaw = r.yaw;
    out.roll = r.roll;
    return out;
}

ue::FRotator to_frot(const Rotator& r) {
    return {r.pitch, r.yaw, r.roll};
}

ue::UObject* object_property(ue::UObject* obj, const char* name) {
    if (obj == nullptr) {
        return nullptr;
    }
    const int32_t offset = ue::property_offset(ue::class_of(obj), name);
    if (offset < 0) {
        return nullptr;
    }
    ue::UObject* value = ue::field<ue::UObject*>(obj, offset);
    return ue::is_live(value) ? value : nullptr;
}

bool component_pose(ue::UObject* component, Vec3* location, Rotator* rotation) {
    ue::Call loc(component, "K2_GetComponentLocation");
    ue::Call rot(component, "K2_GetComponentRotation");
    if (!loc.run() || !rot.run()) {
        return false;
    }
    *location = to_vec3(loc.get<ue::FVector>("ReturnValue"));
    *rotation = to_rot(rot.get<ue::FRotator>("ReturnValue"));
    return true;
}

// Coordinates of `v` in the frame (f, r, u).
Vec3 in_frame(Vec3 v, Vec3 f, Vec3 r, Vec3 u) {
    return {dot(v, f), dot(v, r), dot(v, u)};
}

Vec3 from_frame(Vec3 c, Vec3 f, Vec3 r, Vec3 u) {
    return f * c.x + r * c.y + u * c.z;
}

}  // namespace

void weapon_before_draw(ue::UObject* pc, const HandPose& hand, double body_yaw, double scale) {
    g_arms = nullptr;
    g_pawn = nullptr;
    g_aim_valid = false;
    if (pc == nullptr || !hand.valid) {
        return;
    }
    ue::UObject* pawn = object_property(pc, "Pawn");
    ue::UObject* camera = object_property(pawn, "FirstPersonCamera");
    ue::UObject* arms = object_property(pawn, "FirstPersonArms");
    // Only the gun is shown in the hand: the arms (and the gloves that follow them) would
    // stretch away from the body. Children (the guns) keep their own visibility.
    if (config().hide_arms) {
        for (const char* name : {"FirstPersonArms", "SK_Gloves2B"}) {
            if (ue::UObject* mesh = object_property(pawn, name)) {
                ue::Call hide(mesh, "SetVisibility");
                hide.set<bool>("bNewVisibility", false).set<bool>("bPropagateToChildren", false).run();
            }
        }
    }
    if (camera == nullptr || arms == nullptr) {
        return;
    }
    Vec3 cam_pos, arms_pos;
    Rotator cam_rot, arms_rot;
    if (!component_pose(camera, &cam_pos, &cam_rot) || !component_pose(arms, &arms_pos, &arms_rot)) {
        return;
    }
    g_pawn = pawn;
    g_camera = cam_pos;

    // The controller in the world: the camera stands for the reference head position.
    const double yaw = body_yaw * kDegToRad;
    const Vec3 hand_pos = cam_pos + yaw_ue(hand.offset * scale, yaw);
    Vec3 hf = yaw_ue(hand.forward, yaw), hr = yaw_ue(hand.right, yaw), hu = yaw_ue(hand.up, yaw);
    if (config().gun_pitch != 0.0f) {
        const double p = config().gun_pitch * kDegToRad;
        const Vec3 f = hf * std::cos(p) + hu * std::sin(p);
        hu = hu * std::cos(p) - hf * std::sin(p);
        hf = f;
    }

    // Aim: the game shoots along its control rotation.
    const Rotator aim = axes_to_rotator(hf, hr, hu);
    ue::Call set_aim(pc, "SetControlRotation");
    set_aim.set("NewRotation", ue::FRotator{aim.pitch, aim.yaw, 0.0}).run();
    g_aim_valid = true;
    g_aim_origin = to_fvector(hand_pos);
    g_aim_direction = to_fvector(hf);

    // GunYaw straightens the model on the controller (visual only; the aim stays on hf).
    if (config().gun_yaw != 0.0f) {
        const double y = config().gun_yaw * kDegToRad;
        const Vec3 f = hf * std::cos(y) + hr * std::sin(y);
        hr = hr * std::cos(y) - hf * std::sin(y);
        hf = f;
    }
    // The arms keep their pose relative to the game's view, with the view's hold point
    // (GunOffset) moved onto the controller.
    Vec3 cf, cr, cu, af, ar, au;
    rotator_axes(cam_rot, &cf, &cr, &cu);
    rotator_axes(arms_rot, &af, &ar, &au);
    const Vec3 rel_pos = in_frame(arms_pos - cam_pos, cf, cr, cu);
    const Vec3 rel_f = in_frame(af, cf, cr, cu);
    const Vec3 rel_r = in_frame(ar, cf, cr, cu);
    const Vec3 rel_u = in_frame(au, cf, cr, cu);
    const Vec3 hold{config().gun_x, config().gun_y, config().gun_z};
    const Vec3 view_pos = hand_pos - from_frame(hold, hf, hr, hu);
    Vec3 new_pos = view_pos + from_frame(rel_pos, hf, hr, hu);
    // The arms mesh starts at the shoulders: moved too far from where the game keeps it, the
    // arms float away from the body. ArmsMaxOffset bounds it (the gun then trails the hand).
    const Vec3 delta = new_pos - arms_pos;
    const double max_offset = config().arms_max_offset;
    if (max_offset > 0.0 && length(delta) > max_offset) {
        new_pos = arms_pos + delta * (max_offset / length(delta));
    }
    const Rotator new_rot = axes_to_rotator(from_frame(rel_f, hf, hr, hu), from_frame(rel_r, hf, hr, hu),
                                            from_frame(rel_u, hf, hr, hu));

    const int32_t loc_off = ue::property_offset(ue::class_of(arms), "RelativeLocation");
    const int32_t rot_off = ue::property_offset(ue::class_of(arms), "RelativeRotation");
    if (loc_off < 0 || rot_off < 0) {
        return;
    }
    g_saved_location = ue::field<ue::FVector>(arms, loc_off);
    g_saved_rotation = ue::field<ue::FRotator>(arms, rot_off);
    ue::Call move(arms, "K2_SetWorldLocationAndRotation");
    move.set("NewLocation", to_fvector(new_pos)).set("NewRotation", to_frot(new_rot));
    if (move.run()) {
        g_arms = arms;
    }
    static bool logged = false;
    if (!logged) {
        logged = true;
        LOG("Weapon: arms moved from (%.0f %.0f %.0f) to (%.0f %.0f %.0f), camera (%.0f %.0f %.0f)",
            arms_pos.x, arms_pos.y, arms_pos.z, new_pos.x, new_pos.y, new_pos.z, cam_pos.x, cam_pos.y,
            cam_pos.z);
    }
}

bool weapon_trace(double max_distance, Vec3* hit, Vec3* camera) {
    if (!g_aim_valid || g_pawn == nullptr || !ue::is_live(g_pawn)) {
        return false;
    }
    static ue::UObject* library = ue::find_default_object("KismetSystemLibrary");
    // The game fires along its control rotation (the aim) from the camera, not from the hand.
    const Vec3 start = g_camera;
    const Vec3 dir = to_vec3(g_aim_direction);
    const Vec3 end = start + dir * max_distance;
    ue::Call trace(library, "LineTraceSingle");
    trace.set("WorldContextObject", g_pawn)
        .set("Start", to_fvector(start))
        .set("End", to_fvector(end))
        .set<uint8_t>("TraceChannel", 0)  // TraceTypeQuery1: Visibility
        .set<bool>("bTraceComplex", false)
        .set<bool>("bIgnoreSelf", true);
    if (!trace.run()) {
        return false;
    }
    double distance = max_distance;
    if (trace.get<bool>("ReturnValue")) {
        // FHitResult: FaceIndex, Time, Distance (float at +8).
        const auto& hit_result = trace.get<std::array<uint8_t, 16>>("OutHit");
        float d = 0.0f;
        memcpy(&d, hit_result.data() + 8, sizeof(d));
        if (d > 1.0f && d < max_distance) {
            distance = d;
        }
    }
    *hit = start + dir * distance;
    *camera = g_camera;
    return true;
}

void weapon_after_draw() {
    if (g_arms == nullptr || !ue::is_live(g_arms)) {
        g_arms = nullptr;
        return;
    }
    ue::Call restore(g_arms, "K2_SetRelativeLocationAndRotation");
    restore.set("NewLocation", g_saved_location).set("NewRotation", g_saved_rotation).run();
    g_arms = nullptr;
}

bool weapon_camera(Vec3* camera) {
    if (!g_aim_valid) {
        return false;
    }
    *camera = g_camera;
    return true;
}

bool weapon_aim(ue::FVector* origin, ue::FVector* direction) {
    if (!g_aim_valid) {
        return false;
    }
    *origin = g_aim_origin;
    *direction = g_aim_direction;
    return true;
}

}  // namespace vvr
