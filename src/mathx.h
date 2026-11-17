#pragma once
// Conversions between OpenXR (right-handed, +Y up, -Z forward, metres) and Unreal Engine 5
// (left-handed, +Z up, +X forward, +Y right; double vectors and rotators in degrees).
#include <cmath>
#include <cstdint>

#include <openxr/openxr.h>

namespace vvr::mathx {

constexpr double kPi = 3.14159265358979323846;
constexpr double kDegToRad = kPi / 180.0;
constexpr double kRadToDeg = 180.0 / kPi;

struct Vec3 {
    double x = 0, y = 0, z = 0;
};

inline Vec3 operator+(Vec3 a, Vec3 b) {
    return {a.x + b.x, a.y + b.y, a.z + b.z};
}
inline Vec3 operator-(Vec3 a, Vec3 b) {
    return {a.x - b.x, a.y - b.y, a.z - b.z};
}
inline Vec3 operator*(Vec3 a, double s) {
    return {a.x * s, a.y * s, a.z * s};
}
inline double dot(Vec3 a, Vec3 b) {
    return a.x * b.x + a.y * b.y + a.z * b.z;
}
inline Vec3 cross(Vec3 a, Vec3 b) {
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
inline double length(Vec3 a) {
    return std::sqrt(dot(a, a));
}

inline Vec3 to_vec(const XrVector3f& v) {
    return {v.x, v.y, v.z};
}
inline XrVector3f to_xr(Vec3 v) {
    return {static_cast<float>(v.x), static_cast<float>(v.y), static_cast<float>(v.z)};
}

inline Vec3 rotate(const XrQuaternionf& q, Vec3 v) {
    // v' = v + 2w(u x v) + 2u x (u x v)
    const Vec3 u{q.x, q.y, q.z};
    const Vec3 t = cross(u, v) * 2.0;
    return v + t * q.w + cross(u, t);
}

inline XrQuaternionf quat_mul(const XrQuaternionf& a, const XrQuaternionf& b) {
    return {a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
            a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
            a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w,
            a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z};
}

// Rotation about the OpenXR +Y axis (positive turns -Z towards -X, i.e. left).
inline XrQuaternionf quat_yaw(double radians) {
    return {0.0f, static_cast<float>(std::sin(radians * 0.5)), 0.0f,
            static_cast<float>(std::cos(radians * 0.5))};
}
// Rotation about the OpenXR +X axis (positive tilts -Z up).
inline XrQuaternionf quat_pitch(double radians) {
    return {static_cast<float>(std::sin(radians * 0.5)), 0.0f, 0.0f,
            static_cast<float>(std::cos(radians * 0.5))};
}

// OpenXR vector (metres) -> Unreal axes (same unit).
inline Vec3 xr_to_ue(Vec3 v) {
    return {-v.z, v.x, v.y};
}

// Rotation about the Unreal Z axis by `yaw` radians (positive turns +X towards +Y, i.e. right).
inline Vec3 yaw_ue(Vec3 v, double yaw) {
    const double c = std::cos(yaw), s = std::sin(yaw);
    return {v.x * c - v.y * s, v.x * s + v.y * c, v.z};
}

// Rotation about the OpenXR Y axis by `yaw` radians (positive turns -Z towards -X, i.e. left).
inline Vec3 yaw_xr(Vec3 v, double yaw) {
    const double c = std::cos(yaw), s = std::sin(yaw);
    return {v.x * c + v.z * s, v.y, -v.x * s + v.z * c};
}

// Yaw of an OpenXR orientation around +Y, radians, 0 = looking down -Z, positive = left.
inline double xr_yaw(const XrQuaternionf& q) {
    const Vec3 f = rotate(q, {0, 0, -1});
    return std::atan2(-f.x, -f.z);
}

// Unreal FRotator (degrees): pitch up, yaw right, roll.
struct Rotator {
    double pitch = 0, yaw = 0, roll = 0;
};

// Rotator from orthonormal forward/right/up axes in Unreal space (FMatrix::Rotator()).
inline Rotator axes_to_rotator(Vec3 forward, Vec3 right, Vec3 up) {
    Rotator r;
    const double pitch = std::atan2(forward.z, std::sqrt(forward.x * forward.x + forward.y * forward.y));
    const double yaw = std::atan2(forward.y, forward.x);
    const Vec3 sy{-std::sin(yaw), std::cos(yaw), 0.0};
    const double roll = std::atan2(dot(up, sy), dot(right, sy));
    r.pitch = pitch * kRadToDeg;
    r.yaw = yaw * kRadToDeg;
    r.roll = roll * kRadToDeg;
    return r;
}

// Axes of an Unreal rotator (FRotationMatrix): X forward, Y right, Z up.
inline void rotator_axes(const Rotator& r, Vec3* forward, Vec3* right, Vec3* up) {
    const double sp = std::sin(r.pitch * kDegToRad), cp = std::cos(r.pitch * kDegToRad);
    const double sy = std::sin(r.yaw * kDegToRad), cy = std::cos(r.yaw * kDegToRad);
    const double sr = std::sin(r.roll * kDegToRad), cr = std::cos(r.roll * kDegToRad);
    if (forward) *forward = {cp * cy, cp * sy, sp};
    if (right) *right = {sr * sp * cy - cr * sy, sr * sp * sy + cr * cy, -sr * cp};
    if (up) *up = {-(cr * sp * cy + sr * sy), cy * sr - cr * sp * sy, cr * cp};
}

inline double wrap_degrees(double d) {
    d = std::fmod(d + 180.0, 360.0);
    if (d < 0) d += 360.0;
    return d - 180.0;
}

}  // namespace vvr::mathx
