#pragma once
// Small vector and rotation helpers shared by the plugin's files (game units, Unreal rotators).

#include "bakkesmod/wrappers/includes.h"

#include <algorithm>
#include <cmath>
#include <string>

namespace lab {

constexpr float PI = 3.14159265f;
constexpr float ROT_TO_RAD = PI / 32768.f;
constexpr float RAD_TO_ROT = 32768.f / PI;

inline Vector Add(const Vector& a, const Vector& b) { return Vector(a.X + b.X, a.Y + b.Y, a.Z + b.Z); }
inline Vector Sub(const Vector& a, const Vector& b) { return Vector(a.X - b.X, a.Y - b.Y, a.Z - b.Z); }
inline Vector Mul(const Vector& a, float s) { return Vector(a.X * s, a.Y * s, a.Z * s); }
inline Vector Lerp(const Vector& a, const Vector& b, float k) { return Add(a, Mul(Sub(b, a), k)); }
inline float Dot(const Vector& a, const Vector& b) { return a.X * b.X + a.Y * b.Y + a.Z * b.Z; }
inline float Len(const Vector& a) { return std::sqrt(Dot(a, a)); }

struct Axes {
    Vector f, r, u;
};

// Same as Unreal's FRotationMatrix
inline Axes AxesOf(const Rotator& rot) {
    const float p = rot.Pitch * ROT_TO_RAD, y = rot.Yaw * ROT_TO_RAD, r = rot.Roll * ROT_TO_RAD;
    const float sp = std::sin(p), cp = std::cos(p), sy = std::sin(y), cy = std::cos(y), sr = std::sin(r), cr = std::cos(r);
    return {Vector(cp * cy, cp * sy, sp),
            Vector(sr * sp * cy - cr * sy, sr * sp * sy + cr * cy, -sr * cp),
            Vector(-(cr * sp * cy + sr * sy), cy * sr - cr * sp * sy, cr * cp)};
}

// replay quaternion (x, y, z, w) -> Unreal rotator
inline Rotator ToRotator(const float q[4]) {
    const float x = q[0], y = q[1], z = q[2], w = q[3];
    const float fx = 1 - 2 * (y * y + z * z), fy = 2 * (x * y + w * z), fz = 2 * (x * z - w * y);
    const float rz = 2 * (y * z + w * x), uz = 1 - 2 * (x * x + y * y);
    return Rotator{static_cast<int>(std::lround(std::asin(std::min(std::max(fz, -1.f), 1.f)) * RAD_TO_ROT)),
                   static_cast<int>(std::lround(std::atan2(fy, fx) * RAD_TO_ROT)),
                   static_cast<int>(std::lround(std::atan2(-rz, uz) * RAD_TO_ROT))};
}

// Unreal rotator -> quaternion (x, y, z, w) of the same convention as replays: the inverse of ToRotator
inline void ToQuat(const Rotator& rot, float q[4]) {
    const Axes a = AxesOf(rot);
    // rotation matrix with the forward, right and up axes as columns
    const float m00 = a.f.X, m10 = a.f.Y, m20 = a.f.Z, m01 = a.r.X, m11 = a.r.Y, m21 = a.r.Z, m02 = a.u.X, m12 = a.u.Y,
                m22 = a.u.Z;
    const float trace = m00 + m11 + m22;
    float x, y, z, w;
    if (trace > 0) {
        const float s = std::sqrt(trace + 1.f) * 2.f;
        w = 0.25f * s, x = (m21 - m12) / s, y = (m02 - m20) / s, z = (m10 - m01) / s;
    } else if (m00 > m11 && m00 > m22) {
        const float s = std::sqrt(1.f + m00 - m11 - m22) * 2.f;
        w = (m21 - m12) / s, x = 0.25f * s, y = (m01 + m10) / s, z = (m02 + m20) / s;
    } else if (m11 > m22) {
        const float s = std::sqrt(1.f + m11 - m00 - m22) * 2.f;
        w = (m02 - m20) / s, x = (m01 + m10) / s, y = 0.25f * s, z = (m12 + m21) / s;
    } else {
        const float s = std::sqrt(1.f + m22 - m00 - m11) * 2.f;
        w = (m10 - m01) / s, x = (m02 + m20) / s, y = (m12 + m21) / s, z = 0.25f * s;
    }
    q[0] = x, q[1] = y, q[2] = z, q[3] = w;
}

// the rotator looking from `from` toward `to` (no roll)
inline Rotator LookAt(const Vector& from, const Vector& to) {
    const Vector d = Sub(to, from);
    const float flat = std::sqrt(d.X * d.X + d.Y * d.Y);
    return Rotator{static_cast<int>(std::atan2(d.Z, flat) * RAD_TO_ROT), static_cast<int>(std::atan2(d.Y, d.X) * RAD_TO_ROT), 0};
}

// a car's player name; none for a car left without a player (its owner can be gone: reading it crashed the game)
inline std::string NameOf(CarWrapper car) {
    PriWrapper pri = car.GetPRI();
    return pri.IsNull() ? std::string() : pri.GetPlayerName().ToString();
}

}  // namespace lab
