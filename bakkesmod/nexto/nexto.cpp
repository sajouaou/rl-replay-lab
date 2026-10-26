#include "nexto.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <fstream>

namespace nexto {

const double PAD_LOCATIONS[PADS][3] = {
    {0.0, -4240.0, 70.0},     {-1792.0, -4184.0, 70.0}, {1792.0, -4184.0, 70.0}, {-3072.0, -4096.0, 73.0},
    {3072.0, -4096.0, 73.0},  {-940.0, -3308.0, 70.0},  {940.0, -3308.0, 70.0},   {0.0, -2816.0, 70.0},
    {-3584.0, -2484.0, 70.0}, {3584.0, -2484.0, 70.0},  {-1788.0, -2300.0, 70.0}, {1788.0, -2300.0, 70.0},
    {-2048.0, -1036.0, 70.0}, {0.0, -1024.0, 70.0},     {2048.0, -1036.0, 70.0},  {-3584.0, 0.0, 73.0},
    {-1024.0, 0.0, 70.0},     {1024.0, 0.0, 70.0},      {3584.0, 0.0, 73.0},      {-2048.0, 1036.0, 70.0},
    {0.0, 1024.0, 70.0},      {2048.0, 1036.0, 70.0},   {-1788.0, 2300.0, 70.0},  {1788.0, 2300.0, 70.0},
    {-3584.0, 2484.0, 70.0},  {3584.0, 2484.0, 70.0},   {0.0, 2816.0, 70.0},      {-940.0, 3310.0, 70.0},
    {940.0, 3308.0, 70.0},    {-3072.0, 4096.0, 73.0},  {3072.0, 4096.0, 73.0},   {-1792.0, 4184.0, 70.0},
    {1792.0, 4184.0, 70.0},   {0.0, 4240.0, 70.0},
};

namespace {
// entity features (nexto_obs.py)
enum { IS_SELF, IS_MATE, IS_OPP, IS_BALL, IS_BOOST, POS = 5, LIN_VEL = 8, FW = 11, UP = 14, ANG_VEL = 17,
       BOOST = 20, DEMO, ON_GROUND, HAS_FLIP, FEATURES = 24, QUERY = 32 };
constexpr int DIM = 128, HEADS = 4, HEAD_DIM = DIM / HEADS, HIDDEN = 512, EMB = 32;

void Put(double* row, int at, const Vec& v) {
    row[at] = v.x;
    row[at + 1] = v.y;
    row[at + 2] = v.z;
}

// y = W x + b, W: out x in (PyTorch's layout)
void Linear(const float* W, const float* b, const float* x, int in, int out, float* y) {
    for (int o = 0; o < out; o++) {
        const float* row = W + static_cast<size_t>(o) * in;
        float s = b[o];
        for (int i = 0; i < in; i++) s += row[i] * x[i];
        y[o] = s;
    }
}

void Relu(float* x, int n) {
    for (int i = 0; i < n; i++) x[i] = std::max(x[i], 0.f);
}

void LayerNorm(const float* x, const float* g, const float* b, float* y) {
    float mean = 0;
    for (int i = 0; i < DIM; i++) mean += x[i];
    mean /= DIM;
    float var = 0;
    for (int i = 0; i < DIM; i++) var += (x[i] - mean) * (x[i] - mean);
    var /= DIM;
    const float inv = 1.f / std::sqrt(var + 1e-5f);
    for (int i = 0; i < DIM; i++) y[i] = (x[i] - mean) * inv * g[i] + b[i];
}
}  // namespace

const std::vector<Action>& Model::Table() {
    // agent.py's make_lookup_table
    static const std::vector<Action> table = [] {
        std::vector<Action> t;
        for (int throttle : {-1, 0, 1})
            for (int steer : {-1, 0, 1})
                for (int boost : {0, 1})
                    for (int handbrake : {0, 1}) {
                        if (boost == 1 && throttle != 1) continue;
                        t.push_back({float(throttle), float(steer), 0, float(steer), 0, 0, float(boost), float(handbrake)});
                    }
        for (int pitch : {-1, 0, 1})
            for (int yaw : {-1, 0, 1})
                for (int roll : {-1, 0, 1})
                    for (int jump : {0, 1})
                        for (int boost : {0, 1}) {
                            if (jump == 1 && yaw != 0) continue;            // only roll for a sideflip
                            if (pitch == 0 && roll == 0 && jump == 0) continue;   // same as on the ground
                            const bool handbrake = jump == 1 && (pitch != 0 || yaw != 0 || roll != 0);   // wavedashes
                            t.push_back({float(boost), float(yaw), float(pitch), float(yaw), float(roll), float(jump),
                                         float(boost), float(handbrake)});
                        }
        return t;
    }();
    return table;
}

void Observe(const std::vector<Car>& cars, int me, const Ball& ball, const float pads[PADS], const Action& previous,
             std::vector<float>& q, std::vector<float>& kv) {
    // players: Nexto's car, its teammates, then its opponents; then the ball; then the boost pads
    std::vector<const Car*> players{&cars[me]};
    for (size_t i = 0; i < cars.size(); i++)
        if (static_cast<int>(i) != me && cars[i].team == cars[me].team) players.push_back(&cars[i]);
    for (const Car& c : cars)
        if (c.team != cars[me].team) players.push_back(&c);
    const int n = static_cast<int>(players.size()), entities = n + 1 + PADS;
    std::vector<double> rows(static_cast<size_t>(entities) * FEATURES, 0.0);
    for (int i = 0; i < n; i++) {
        const Car& c = *players[i];
        double* r = &rows[static_cast<size_t>(i) * FEATURES];
        r[IS_SELF] = i == 0;
        r[IS_MATE] = 1 - c.team;   // relative to blue: swapped below when Nexto is orange
        r[IS_OPP] = c.team;
        Put(r, POS, c.pos);
        Put(r, LIN_VEL, c.vel);
        Put(r, FW, c.fwd);
        Put(r, UP, c.up);
        Put(r, ANG_VEL, c.ang);
        r[BOOST] = c.boost;
        r[DEMO] = c.demoed;
        r[ON_GROUND] = c.onGround;
        r[HAS_FLIP] = c.hasFlip;
    }
    double* b = &rows[static_cast<size_t>(n) * FEATURES];
    b[IS_BALL] = 1;
    Put(b, POS, ball.pos);
    Put(b, LIN_VEL, ball.vel);
    Put(b, ANG_VEL, ball.ang);
    for (int p = 0; p < PADS; p++) {
        double* r = &rows[static_cast<size_t>(n + 1 + p) * FEATURES];
        r[IS_BOOST] = 1;
        Put(r, POS, Vec{PAD_LOCATIONS[p][0], PAD_LOCATIONS[p][1], PAD_LOCATIONS[p][2]});
        r[BOOST] = PAD_LOCATIONS[p][2] > 72 ? 1.0 : 0.12;
        r[DEMO] = pads[p];
    }
    const bool orange = cars[me].team == 1;
    for (int e = 0; e < entities; e++) {
        double* r = &rows[static_cast<size_t>(e) * FEATURES];
        if (orange) {   // seen from the orange side: the field turned around, teammates and opponents swapped
            for (int v = POS; v < BOOST; v += 3) {
                r[v] = -r[v];
                r[v + 1] = -r[v + 1];
            }
            std::swap(r[IS_MATE], r[IS_OPP]);
        }
        for (int f = POS; f < FW; f++) r[f] /= 2300.0;
        for (int f = ANG_VEL; f < BOOST; f++) r[f] /= 5.5;
    }
    std::vector<double> self(rows.begin(), rows.begin() + FEATURES);
    q.assign(QUERY, 0.f);
    for (int f = 0; f < FEATURES; f++) q[f] = static_cast<float>(self[f]);
    for (int a = 0; a < 8; a++) q[FEATURES + a] = previous[a];
    // everything else relative to Nexto's car: its position, and turned to its heading
    const double theta = std::atan2(self[FW], self[FW + 1]), ct = std::cos(theta), st = std::sin(theta);
    kv.assign(static_cast<size_t>(entities) * FEATURES, 0.f);
    for (int e = 0; e < entities; e++) {
        double* r = &rows[static_cast<size_t>(e) * FEATURES];
        for (int f = POS; f < LIN_VEL; f++) r[f] -= self[f];
        for (int v = POS; v < BOOST; v += 3) {
            const double x = r[v], y = r[v + 1];
            r[v] = ct * x - st * y;
            r[v + 1] = st * x + ct * y;
        }
        for (int f = 0; f < FEATURES; f++) kv[static_cast<size_t>(e) * FEATURES + f] = static_cast<float>(r[f]);
    }
}

bool Model::Load(const std::filesystem::path& path, std::string& error) {
    static const int SIZES[] = {
        DIM * QUERY, DIM, DIM * DIM, DIM,              // query_preprocess
        DIM * FEATURES, DIM, DIM * DIM, DIM,           // key_value_preprocess
        3 * DIM * DIM, 3 * DIM, DIM * DIM, DIM, HIDDEN * DIM, HIDDEN, DIM * HIDDEN, DIM, DIM, DIM, DIM, DIM, DIM, DIM,   // block 0
        3 * DIM * DIM, 3 * DIM, DIM * DIM, DIM, HIDDEN * DIM, HIDDEN, DIM * HIDDEN, DIM, DIM, DIM, DIM, DIM, DIM, DIM,   // block 1
        EMB * 8, EMB, EMB * EMB, EMB, EMB * EMB, EMB,  // output network (the actions)
        EMB * DIM, EMB,                                // emb_convertor
    };
    constexpr uint32_t COUNT = sizeof(SIZES) / sizeof(SIZES[0]);
    std::ifstream in(path, std::ios::binary);
    char magic[4] = {};
    uint32_t count = 0;
    if (!in.read(magic, 4) || std::memcmp(magic, "NXTO", 4) != 0 || !in.read(reinterpret_cast<char*>(&count), 4) || count != COUNT) {
        error = "fichier de Nexto invalide: " + path.string();
        return false;
    }
    std::vector<std::vector<float>> tensors(COUNT);
    for (uint32_t t = 0; t < COUNT; t++) {
        uint32_t size = 0;
        if (!in.read(reinterpret_cast<char*>(&size), 4) || size != static_cast<uint32_t>(SIZES[t])) {
            error = "fichier de Nexto incomplet: " + path.string();
            return false;
        }
        tensors[t].resize(size);
        if (!in.read(reinterpret_cast<char*>(tensors[t].data()), static_cast<std::streamsize>(size) * 4)) {
            error = "fichier de Nexto incomplet: " + path.string();
            return false;
        }
    }
    w = std::move(tensors);
    // the actions do not depend on the game: run them through the output network once
    const int out = 36;   // first tensor of the output network
    actionEmb.assign(ACTIONS * EMB, 0.f);
    for (int a = 0; a < ACTIONS; a++) {
        float h1[EMB], h2[EMB];
        Linear(w[out].data(), w[out + 1].data(), Table()[a].data(), 8, EMB, h1);
        Relu(h1, EMB);
        Linear(w[out + 2].data(), w[out + 3].data(), h1, EMB, EMB, h2);
        Relu(h2, EMB);
        Linear(w[out + 4].data(), w[out + 5].data(), h2, EMB, EMB, &actionEmb[a * EMB]);
        Relu(&actionEmb[a * EMB], EMB);
    }
    return true;
}

void Model::Scores(const std::vector<float>& q, const std::vector<float>& kv, float scores[ACTIONS]) const {
    const int entities = static_cast<int>(kv.size() / FEATURES);
    // preprocessing: two layers each for the query (Nexto's car) and the keys/values (every entity)
    float x[DIM], h[DIM];
    Linear(w[0].data(), w[1].data(), q.data(), QUERY, DIM, h);
    Relu(h, DIM);
    Linear(w[2].data(), w[3].data(), h, DIM, DIM, x);
    Relu(x, DIM);
    std::vector<float> keys(static_cast<size_t>(entities) * DIM);
    for (int e = 0; e < entities; e++) {
        Linear(w[4].data(), w[5].data(), &kv[static_cast<size_t>(e) * FEATURES], FEATURES, DIM, h);
        Relu(h, DIM);
        Linear(w[6].data(), w[7].data(), h, DIM, DIM, &keys[static_cast<size_t>(e) * DIM]);
        Relu(&keys[static_cast<size_t>(e) * DIM], DIM);
    }
    std::vector<float> kn(static_cast<size_t>(entities) * DIM), K(kn.size()), V(kn.size()), att(entities);
    float qn[DIM], Q[DIM], o[DIM], y[DIM], hidden[HIDDEN];
    for (int blk = 0; blk < 2; blk++) {
        const std::vector<float>* p = &w[8 + blk * 14];
        const float *inW = p[0].data(), *inB = p[1].data();
        // attention of Nexto's car (query) over every entity (keys and values), 4 heads
        LayerNorm(x, p[8].data(), p[9].data(), qn);
        Linear(inW, inB, qn, DIM, DIM, Q);
        for (int e = 0; e < entities; e++) {
            float* row = &kn[static_cast<size_t>(e) * DIM];
            LayerNorm(&keys[static_cast<size_t>(e) * DIM], p[10].data(), p[11].data(), row);
            Linear(inW + DIM * DIM, inB + DIM, row, DIM, DIM, &K[static_cast<size_t>(e) * DIM]);
            Linear(inW + 2 * DIM * DIM, inB + 2 * DIM, row, DIM, DIM, &V[static_cast<size_t>(e) * DIM]);
        }
        const float scale = 1.f / std::sqrt(static_cast<float>(HEAD_DIM));
        for (int hd = 0; hd < HEADS; hd++) {
            const int at = hd * HEAD_DIM;
            float top = -1e30f;
            for (int e = 0; e < entities; e++) {
                float s = 0;
                for (int d = 0; d < HEAD_DIM; d++) s += Q[at + d] * scale * K[static_cast<size_t>(e) * DIM + at + d];
                att[e] = s;   // no padding: the mask adds nothing
                top = std::max(top, s);
            }
            float sum = 0;
            for (int e = 0; e < entities; e++) sum += att[e] = std::exp(att[e] - top);
            for (int d = 0; d < HEAD_DIM; d++) {
                float s = 0;
                for (int e = 0; e < entities; e++) s += att[e] * V[static_cast<size_t>(e) * DIM + at + d];
                o[at + d] = s / sum;
            }
        }
        Linear(p[2].data(), p[3].data(), o, DIM, DIM, y);
        for (int i = 0; i < DIM; i++) x[i] += y[i];
        // feed-forward
        LayerNorm(x, p[12].data(), p[13].data(), qn);
        Linear(p[4].data(), p[5].data(), qn, DIM, HIDDEN, hidden);
        Relu(hidden, HIDDEN);
        Linear(p[6].data(), p[7].data(), hidden, HIDDEN, DIM, y);
        for (int i = 0; i < DIM; i++) x[i] += y[i];
    }
    Relu(x, DIM);
    float emb[EMB];
    Linear(w[42].data(), w[43].data(), x, DIM, EMB, emb);
    for (int a = 0; a < ACTIONS; a++) {
        float s = 0;
        for (int d = 0; d < EMB; d++) s += actionEmb[a * EMB + d] * emb[d];
        scores[a] = s;
    }
}

int Model::Act(const std::vector<Car>& cars, int me, const Ball& ball, const float pads[PADS], const Action& previous) const {
    std::vector<float> q, kv;
    Observe(cars, me, ball, pads, previous, q, kv);
    float scores[ACTIONS];
    Scores(q, kv, scores);
    return static_cast<int>(std::max_element(scores, scores + ACTIONS) - scores);
}

}  // namespace nexto
