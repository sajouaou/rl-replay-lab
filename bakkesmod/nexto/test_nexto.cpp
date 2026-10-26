// Checks nexto.cpp against the original Nexto (Python + PyTorch) on states taken from a replay:
//   python export_nexto.py <blob> <dir>; python -c ... (nexto_test.txt); test_nexto <dir>/nexto.bin <dir>/nexto_test.txt
#include "nexto.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <fstream>

int main(int argc, char** argv) {
    if (argc < 3) return 2;
    nexto::Model model;
    std::string error;
    if (!model.Load(argv[1], error)) {
        std::printf("%s\n", error.c_str());
        return 1;
    }
    std::ifstream in(argv[2]);
    int samples = 0, sameAction = 0;
    double worstQ = 0, worstKv = 0, worstScore = 0, totalMs = 0;
    int n;
    while (in >> n) {
        std::vector<nexto::Car> cars(n);
        auto vec = [&](nexto::Vec& v) { in >> v.x >> v.y >> v.z; };
        for (auto& c : cars) {
            int demoed, ground, flip;
            in >> c.team;
            vec(c.pos), vec(c.vel), vec(c.ang), vec(c.fwd), vec(c.up);
            in >> c.boost >> demoed >> ground >> flip;
            c.demoed = demoed, c.onGround = ground, c.hasFlip = flip;
        }
        nexto::Ball ball;
        vec(ball.pos), vec(ball.vel), vec(ball.ang);
        float pads[nexto::PADS];
        for (float& p : pads) in >> p;
        nexto::Action prev;
        for (float& a : prev) in >> a;
        std::vector<float> wantQ(32), wantKv(static_cast<size_t>(n + 1 + nexto::PADS) * 24);
        float wantScores[nexto::ACTIONS];
        int wantAction;
        for (float& v : wantQ) in >> v;
        for (float& v : wantKv) in >> v;
        for (float& v : wantScores) in >> v;
        in >> wantAction;

        std::vector<float> q, kv;
        nexto::Observe(cars, 0, ball, pads, prev, q, kv);
        for (int i = 0; i < 32; i++) worstQ = std::max(worstQ, double(std::fabs(q[i] - wantQ[i])));
        for (size_t i = 0; i < kv.size(); i++) worstKv = std::max(worstKv, double(std::fabs(kv[i] - wantKv[i])));
        const auto t0 = std::chrono::steady_clock::now();
        const int action = model.Act(cars, 0, ball, pads, prev);
        totalMs += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        float scores[nexto::ACTIONS];
        model.Scores(wantQ, wantKv, scores);   // the network alone, on the original observation
        for (int a = 0; a < nexto::ACTIONS; a++) worstScore = std::max(worstScore, double(std::fabs(scores[a] - wantScores[a])));
        sameAction += action == wantAction;
        samples++;
    }
    std::printf("%d observations: meme action %d/%d, ecart max observation q %.2e kv %.2e, scores %.2e, %.2f ms par decision\n",
                samples, sameAction, samples, worstQ, worstKv, worstScore, totalMs / std::max(samples, 1));
    return sameAction == samples ? 0 : 1;
}
