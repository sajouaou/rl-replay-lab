#pragma once
// Nexto inside the plugin, without Python or PyTorch.
// Nexto is the RLGym bot by Rolv-Arild and Soren (RLBotPack, CC BY-NC-SA 4.0, personal non-commercial use).
// This redoes its observation builder (nexto_obs.py) and its network (nexto-model.pt, weights exported to nexto.bin
// by export_nexto.py) in plain C++; test_nexto.cpp checks both against the original model.

#include <array>
#include <filesystem>
#include <string>
#include <vector>

namespace nexto {

struct Vec {
    double x = 0, y = 0, z = 0;
};

// what Nexto sees of a car, in field coordinates (game units, rad/s)
struct Car {
    int team = 0;          // 0: blue, defends the goal at -Y; 1: orange
    Vec pos, vel, ang, fwd, up;
    double boost = 0;      // 0..1
    bool demoed = false, onGround = false, hasFlip = false;
};

struct Ball {
    Vec pos, vel, ang;
};

constexpr int PADS = 34;         // boost pads, in the order Nexto was trained with
constexpr int ACTIONS = 90;
constexpr int TICK_SKIP = 8;     // a new action every 8 physics ticks (15 per second)
extern const double PAD_LOCATIONS[PADS][3];

// throttle, steer, pitch, yaw, roll, jump, boost, handbrake
using Action = std::array<float, 8>;

// The 24 numbers per entity and 32 for the query (24 + the previous action) of nexto_obs.py.
// cars[me] is the car Nexto drives; pads: 1 = available.
void Observe(const std::vector<Car>& cars, int me, const Ball& ball, const float pads[PADS], const Action& previous,
             std::vector<float>& q, std::vector<float>& kv);

class Model {
public:
    bool Load(const std::filesystem::path& path, std::string& error);
    bool Loaded() const { return !w.empty(); }
    // scores of the 90 actions for one observation
    void Scores(const std::vector<float>& q, const std::vector<float>& kv, float scores[ACTIONS]) const;
    // Nexto's best action (its index in Table())
    int Act(const std::vector<Car>& cars, int me, const Ball& ball, const float pads[PADS], const Action& previous) const;
    static const std::vector<Action>& Table();

private:
    std::vector<std::vector<float>> w;   // tensors in export_nexto.py's order
    std::vector<float> actionEmb;        // the 90 actions through the output network: 90 x 32
};

}  // namespace nexto
