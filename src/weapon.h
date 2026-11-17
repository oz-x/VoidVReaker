#pragma once
#include "mathx.h"
#include "ue5.h"

// The weapon in the right hand: the controller aims (the player controller's ControlRotation
// follows it) and the first-person arms, with the gun, are drawn on the controller.
namespace vvr {

// The right controller relative to the recentred reference, in Unreal axes (metres), not yet
// turned by the body yaw.
struct HandPose {
    bool valid = false;
    mathx::Vec3 offset;
    mathx::Vec3 forward, right, up;
};

// Game thread, after the world tick and before the viewport draw. `body_yaw` in degrees.
void weapon_before_draw(ue::UObject* pc, const HandPose& hand, double body_yaw, double scale);
// After the draw: the arms go back where the game keeps them, so its own logic never sees
// the moved transform.
void weapon_after_draw();
// Where the gun points this frame (world, Unreal units), for the reticle.
bool weapon_aim(ue::FVector* origin, ue::FVector* direction);
// What the gun points at (line trace, Visibility channel, up to `max_distance` units) and the
// camera location the hand was placed from.
bool weapon_trace(double max_distance, mathx::Vec3* hit, mathx::Vec3* camera);
// The camera location the hand was placed from this frame (the reference head position).
bool weapon_camera(mathx::Vec3* camera);

}  // namespace vvr
