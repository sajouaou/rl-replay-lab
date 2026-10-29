#pragma once
// The game's rules and the plugin's constants shared by its files: the game modes (their freeplay, ball, goals, arena),
// what a car has in the air, the roles, a few input and paint helpers.
#include "ReplayLab.h"
#include "LabMath.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <string>

namespace lab {
constexpr float TOUCH_DIST = 220.f;       // a car's centre this close to the ball's: touching it (ball 93 + car ~75, margin)
constexpr float PARK_Z = -600.f;          // under the field floor (z = 0)
constexpr float GOAL_Y = 5120.f + 93.f;   // goal line + ball radius: the whole ball is in
constexpr float TICK = 1.f / 120.f;       // one physics step of the game
constexpr int OCTANE = 23;
constexpr float FAST = 3.f;               // the replay run fast (forward or back) with the controller's throttle
constexpr int MAX_SPARE_BOTS = 5;         // bots beyond the replay's players (other bodies, the free camera's car)
constexpr float KICKOFF_COUNTDOWN = 3.f;  // a simulation's kickoff: 3, 2, 1, GO!
// Octane hitbox: half extents and offset from the car's center of mass (game units)
constexpr float HALF_L = 59.f, HALF_W = 42.f, HALF_H = 18.f, OFF_F = 13.88f, OFF_U = 20.75f;
inline const char* const ROLE_LABELS[ROLE_COUNT] = {"Toi (tu la conduis)", "Fantome (refait le vrai trajet)", "Retiree",
                                       "Bot Psyonix", "Libre (sans pilote)", "Nexto (IA RLGym)"};
// each mode's freeplay: the game class loads the mode's package, and its freeplay has the mode's ball
// (soccar keeps the training freeplay Replay Lab was made with)
inline const char* const KIND_CLASS[KIND_COUNT] = {"TAGame.GameInfo_Tutorial_TA", "TAGame.GameInfo_Basketball_TA", "TAGame.GameInfo_Hockey_TA",
                                      "TAGame.GameInfo_Breakout_TA"};
inline const char* const KIND_LABELS[KIND_COUNT] = {"Soccar", "Hoops", "Snow Day", "Dropshot"};

// the mode of a replay, from the ball its game used (Rumble, Heatseeker, mutator balls... play as soccar)
inline int KindOfBall(const std::string& arch) {
    if (arch.find("BasketBall") != std::string::npos) return KIND_HOOPS;
    if (arch.find("Puck") != std::string::npos) return KIND_SNOWDAY;
    if (arch.find("Breakout") != std::string::npos) return KIND_DROPSHOT;
    return KIND_SOCCAR;
}

// the mode a freeplay plays, from its ball (radius measured in the game: 95.5 soccar, 98.1 Hoops, 29 the puck, 102 Dropshot)
inline int KindOfRadius(float r) {
    if (r < 60.f) return KIND_SNOWDAY;
    if (r > 100.f) return KIND_DROPSHOT;
    if (r > 97.f) return KIND_HOOPS;
    return KIND_SOCCAR;
}

// the ball (its center) has scored, by the rules of the mode
inline bool Scored(const Vector& p, int kind) {
    switch (kind) {
    case KIND_HOOPS: {   // down through the ring (RocketSim's rule: the 5 goals of a Hoops replay are inside)
        if (p.Z >= 270.f) return false;
        const float dy = std::fabs(p.Y) * 0.9f - 2770.f;
        return p.X * p.X + dy * dy < 716.f * 716.f;
    }
    case KIND_SNOWDAY: return std::fabs(p.Y) > 5124.25f + 114.25f;   // the whole puck past the line
    case KIND_DROPSHOT: return p.Z < -100.f;                          // through a broken floor panel
    default: return std::fabs(p.Y) > GOAL_Y;
    }
}

constexpr float DODGE_WINDOW = 1.25f;   // seconds after a jump the flip stays

// each mode's arena: half width, half length, height (Dropshot's walls are not straight: its floor only)
struct Arena {
    float x, y, h;
};
inline constexpr Arena ARENAS[KIND_COUNT] = {{4096.f, 5120.f, 2044.f}, {2966.67f, 3581.f, 1820.f}, {4096.f, 5120.f, 2044.f}, {0.f, 0.f, 0.f}};

// a car with its wheels on the floor, a wall or the ceiling: where it gets its jump and flip back
inline bool OnSurface(const RBody& b, int kind) {
    const Vector up = AxesOf(ToRotator(b.quat)).u;
    const Arena& a = ARENAS[kind];
    constexpr float CLOSE = 40.f;   // the car's center sits 17 units off the surface
    if (b.pos.Z < CLOSE && up.Z > 0.7f) return true;
    if (a.h <= 0.f) return false;
    if (b.pos.Z > a.h - CLOSE && up.Z < -0.7f) return true;
    if (std::fabs(b.pos.X) > a.x - CLOSE && up.X * (b.pos.X > 0 ? 1.f : -1.f) < -0.7f) return true;
    return std::fabs(b.pos.Y) > a.y - CLOSE && up.Y * (b.pos.Y > 0 ? 1.f : -1.f) < -0.7f;
}

// the car's wheels against the ball: a flip reset
inline bool WheelsOnBall(const RBody& car, const RBody& ball) {
    if (!car.valid || !ball.valid) return false;
    const Vector d = Sub(ball.pos, car.pos);
    const float l = Len(d);
    return l > 1.f && l < 190.f && Dot(AxesOf(ToRotator(car.quat)).u, d) < -0.5f * l;
}

inline constexpr float SPEEDS[] = {0.25f, 0.5f, 1.f, 2.f};
inline const char* const SPEED_LABELS[] = {"x0.25", "x0.5", "x1", "x2"};

inline void Nlerp(const float a[4], const float b[4], float k, float out[4]) {
    const float sign = (a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3]) < 0 ? -1.f : 1.f;
    float n = 0;
    for (int i = 0; i < 4; i++) {
        out[i] = a[i] * (1 - k) + sign * b[i] * k;
        n += out[i] * out[i];
    }
    n = std::sqrt(std::max(n, 1e-12f));
    for (int i = 0; i < 4; i++) out[i] /= n;
}

// a swatch (0-255) as the colour a car's paint takes: as it is (0-1); rlab_gamma 2.2 would make it linear, which turned
// orange red and purple black
inline LinearColor Paint(const unsigned char c[3], float gamma) {
    auto f = [gamma](unsigned char v) { return std::pow(v / 255.f, gamma); };
    return LinearColor(f(c[0]), f(c[1]), f(c[2]), 1.f);
}

// any stick, trigger or button past a small dead zone
inline bool Moves(const ControllerInput& in) {
    const float dead = 0.2f;
    return std::fabs(in.Throttle) > dead || std::fabs(in.Steer) > dead || std::fabs(in.Pitch) > dead || std::fabs(in.Yaw) > dead ||
           std::fabs(in.Roll) > dead || in.Jump || in.ActivateBoost || in.HoldingBoost || in.Handbrake;
}

inline void ZeroInput(ControllerInput* in) {
    in->Throttle = in->Steer = in->Pitch = in->Yaw = in->Roll = in->DodgeForward = in->DodgeStrafe = 0.f;
    in->Handbrake = in->Jump = in->ActivateBoost = in->HoldingBoost = 0;
}


inline std::string HexId(const std::string& raw) {
    std::string out;
    for (char c : raw)
        if (std::isxdigit(static_cast<unsigned char>(c))) out += static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return out;
}

}  // namespace lab
