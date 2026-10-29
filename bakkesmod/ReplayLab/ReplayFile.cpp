// Replay files: the list of the Demos folder (with the name given in the game), reading one with rlab_replay.dll on a
// worker thread, decoding its blob (format: rlab_replay/src/lib.rs) and smoothing the recorded paths.
#include "ReplayLab.h"
#include "LabMath.h"
#include "Rules.h"

#include <cstring>
#include <filesystem>
#include <sstream>
#include <thread>

#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

using namespace lab;

namespace {
constexpr double SMOOTH = 20.0;          // weight of the recorded velocities against the recorded positions
constexpr float TELEPORT = 250.f;        // this far from where the velocity leads: a teleport, not smoothed across

struct Reader {
    const unsigned char* p;
    const unsigned char* end;
    bool ok = true;
    template <class T> T get() {
        T v{};
        if (p + sizeof(T) > end) { ok = false; return v; }
        std::memcpy(&v, p, sizeof(T));
        p += sizeof(T);
        return v;
    }
    std::string str() {
        const uint16_t n = get<uint16_t>();
        if (!ok || p + n > end) { ok = false; return ""; }
        std::string s(reinterpret_cast<const char*>(p), n);
        p += n;
        return s;
    }
    // one statement per read: function arguments have no set evaluation order (MSVC goes right to left)
    Vector vec() {
        const float x = get<float>();
        const float y = get<float>();
        const float z = get<float>();
        return Vector(x, y, z);
    }
    RBody body() {
        RBody b;
        b.valid = get<uint8_t>() != 0;
        b.pos = vec();
        for (float& q : b.quat) q = get<float>();
        b.vel = vec();
        b.ang = vec();
        return b;
    }
};
// Least squares path through the recorded values r (one per frame) that steps by d between frames:
// minimizes sum (x - r)^2 + SMOOTH * sum (x[q+1] - x[q] - d[q])^2, a tridiagonal system (Thomas algorithm).
void Solve(std::vector<double>& r, const std::vector<double>& d) {
    const size_t m = r.size();
    std::vector<double> di(m, 1.0), up(m, 0.0), lo(m, 0.0);
    for (size_t q = 0; q + 1 < m; q++) {
        di[q] += SMOOTH;
        di[q + 1] += SMOOTH;
        up[q] = lo[q + 1] = -SMOOTH;
        r[q] -= SMOOTH * d[q];
        r[q + 1] += SMOOTH * d[q];
    }
    for (size_t q = 1; q < m; q++) {
        const double w = lo[q] / di[q - 1];
        di[q] -= w * up[q - 1];
        r[q] -= w * r[q - 1];
    }
    r[m - 1] /= di[m - 1];
    for (size_t q = m - 1; q-- > 0;) r[q] = (r[q] - up[q] * r[q + 1]) / di[q];
}

float& Axis(Vector& v, int a) { return a == 0 ? v.X : a == 1 ? v.Y : v.Z; }

// rotation q turned for dt seconds at the world angular velocity w (rad/s)
void Turn(const float q[4], const Vector& w, float dt, float out[4]) {
    const float speed = Len(w);
    if (speed < 1e-6f) {
        std::copy(q, q + 4, out);
        return;
    }
    const float h = speed * dt / 2, k = std::sin(h) / speed;
    const float dx = w.X * k, dy = w.Y * k, dz = w.Z * k, dw = std::cos(h);
    out[0] = dw * q[0] + dx * q[3] + dy * q[2] - dz * q[1];
    out[1] = dw * q[1] - dx * q[2] + dy * q[3] + dz * q[0];
    out[2] = dw * q[2] + dx * q[1] - dy * q[0] + dz * q[3];
    out[3] = dw * q[3] - dx * q[0] - dy * q[1] - dz * q[2];
}

float QuatDot(const float a[4], const float b[4]) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3]; }

// A replay records each car (every 2 or 3 frames) and the ball at uneven moments: frame to frame they surge and
// slow down. Rebuild each path so it moves and turns at the recorded velocities while staying close to the recorded
// positions and rotations (on this kind of replay: about 10 units and 3 degrees away). A teleport is not smoothed across.
void SmoothPath(RBody* first, size_t stride, const std::vector<float>& time) {
    const int n = static_cast<int>(time.size());
    auto at = [&](int i) -> RBody& { return first[static_cast<size_t>(i) * stride]; };
    std::vector<int> seg;
    std::vector<double> x, d;
    for (int i = 0; i < n;) {
        if (!at(i).valid) {
            i++;
            continue;
        }
        seg.assign(1, i);
        int j = i + 1;
        for (; j < n && at(j).valid; j++) {
            RBody& a = at(j - 1);
            RBody& b = at(j);
            const float dt = time[j] - time[j - 1];
            if (QuatDot(a.quat, b.quat) < 0) for (float& c : b.quat) c = -c;   // same rotation, nearer to the last one
            float turned[4];
            Turn(a.quat, Mul(Add(a.ang, b.ang), 0.5f), dt, turned);
            const Vector lead = Add(a.pos, Mul(Add(a.vel, b.vel), 0.5f * dt));
            if (Len(Sub(b.pos, lead)) > TELEPORT || std::fabs(QuatDot(turned, b.quat)) < 0.97f) break;
            seg.push_back(j);
        }
        const size_t m = seg.size();
        if (m >= 3) {
            x.resize(m);
            d.resize(m - 1);
            for (int a = 0; a < 3; a++) {
                for (size_t q = 0; q < m; q++) x[q] = Axis(at(seg[q]).pos, a);
                for (size_t q = 0; q + 1 < m; q++) {
                    RBody& p0 = at(seg[q]);
                    RBody& p1 = at(seg[q + 1]);
                    d[q] = 0.5 * (Axis(p0.vel, a) + Axis(p1.vel, a)) * (time[seg[q + 1]] - time[seg[q]]);
                }
                Solve(x, d);
                for (size_t q = 0; q < m; q++) Axis(at(seg[q]).pos, a) = static_cast<float>(x[q]);
            }
            std::vector<std::array<float, 4>> quats(m);
            for (int c = 0; c < 4; c++) {
                for (size_t q = 0; q < m; q++) x[q] = at(seg[q]).quat[c];
                for (size_t q = 0; q + 1 < m; q++) {
                    RBody& p0 = at(seg[q]);
                    RBody& p1 = at(seg[q + 1]);
                    float turned[4];
                    Turn(p0.quat, Mul(Add(p0.ang, p1.ang), 0.5f), time[seg[q + 1]] - time[seg[q]], turned);
                    d[q] = turned[c] - p0.quat[c];
                }
                Solve(x, d);
                for (size_t q = 0; q < m; q++) quats[q][c] = static_cast<float>(x[q]);
            }
            for (size_t q = 0; q < m; q++) {
                const float l = std::sqrt(std::max(QuatDot(quats[q].data(), quats[q].data()), 1e-12f));
                for (int c = 0; c < 4; c++) at(seg[q]).quat[c] = quats[q][c] / l;
            }
        }
        i = j;
    }
}

void SmoothPaths(ReplayData& R) {
    SmoothPath(R.ball.data(), 1, R.time);
    for (int k = 0; k < R.Players(); k++) SmoothPath(R.cars.data() + k, static_cast<size_t>(R.Players()), R.time);
}
}  // namespace

// A worker thread (reading the replay list, a replay): the previous one is over (joined) before the next, and both are
// joined when the plugin is unloaded, so none runs the plugin's code once it is gone.
void ReplayLab::RunWorker(std::thread& worker, std::function<void()> work) {
    if (worker.joinable()) worker.join();
    worker = std::thread(std::move(work));
}

void ReplayLab::RefreshList() {
    {
        std::lock_guard<std::recursive_mutex> lock(mtx);
        if (listing || !replayHeader) return;
        listing = true;
    }
    const std::wstring folder = DemosFolder();
    const std::string me = *opt.player;
    RunWorker(listWorker, [this, folder, me] {
        std::vector<ReplayEntry> out;
        std::error_code ec;
        for (const auto& e : std::filesystem::directory_iterator(folder, ec)) {
            if (e.path().extension() != L".replay") continue;
            const std::string path = e.path().u8string();
            unsigned char* p = nullptr;
            size_t n = 0;
            char err[256] = {0};
            if (replayHeader(path.c_str(), &p, &n, err, sizeof err) != 0 || !p) continue;
            std::string text(reinterpret_cast<char*>(p), n);
            freeBlob(p, n);
            std::map<std::string, std::string> kv;
            int myTeam = -1;
            std::istringstream lines(text);
            std::string line;
            while (std::getline(lines, line)) {
                const size_t eq = line.find('=');
                if (eq == std::string::npos) continue;
                const std::string key = line.substr(0, eq), value = line.substr(eq + 1);
                if (key == "Player") {
                    const size_t sc = value.rfind(';');
                    if (sc != std::string::npos && value.substr(0, sc) == me) myTeam = std::atoi(value.substr(sc + 1).c_str());
                } else {
                    kv[key] = value;
                }
            }
            ReplayEntry entry;
            entry.id = e.path().stem().u8string();
            entry.path = path;
            entry.map = kv["MapName"];
            entry.date = kv["Date"];
            const int s0 = std::atoi(kv["Team0Score"].c_str()), s1 = std::atoi(kv["Team1Score"].c_str());
            std::string result;
            if (myTeam >= 0) result = (myTeam == 0 ? s0 > s1 : s1 > s0) ? "  victoire" : "  defaite";
            // the name given in the game when it was saved, first
            const std::string& name = kv["ReplayName"];
            entry.label = (name.empty() ? "" : "\"" + name + "\"  ") + entry.date.substr(0, 10) + "  " + kv["TeamSize"] + "v" +
                          kv["TeamSize"] + "  " + entry.map + "  " + std::to_string(s0) + "-" + std::to_string(s1) + result;
            out.push_back(entry);
        }
        std::sort(out.begin(), out.end(), [](const ReplayEntry& a, const ReplayEntry& b) { return a.date > b.date; });
        std::lock_guard<std::recursive_mutex> lock(mtx);
        library = out;
        listing = false;
    });
}

bool PlayerCam::BallCamAt(float t) const {
    bool on = !ballCam.empty() && ballCam.front().second;   // before the first switch: as it started
    for (const auto& [time, value] : ballCam) {
        if (time > t) break;
        on = value;
    }
    return on;
}

int Look::Body() const {
    for (const auto& s : slots)
        if (s[0] == 0 && s[1] > 0) return s[1];
    return OCTANE;
}

bool ReplayLab::Decode(const unsigned char* data, size_t len, ReplayData& out) const {
    Reader r{data, data + len};
    if (len < 8 || std::memcmp(data, "RLAB", 4) != 0) return false;
    r.p += 4;
    const uint32_t version = r.get<uint32_t>();
    if (version < 1 || version > 6) return false;
    out.map = r.str();
    out.date = r.str();
    const uint32_t frames = r.get<uint32_t>(), players = r.get<uint32_t>();
    if (!r.ok || frames == 0 || players > 16) return false;
    for (uint32_t k = 0; k < players; k++) {
        out.names.push_back(r.str());
        out.teams.push_back(r.get<uint8_t>());
    }
    out.time.resize(frames);
    out.secs.resize(frames);
    out.ball.resize(frames);
    out.cars.resize(static_cast<size_t>(frames) * players);
    out.boost.resize(out.cars.size());
    out.boosting.resize(out.cars.size());
    out.moves.resize(out.cars.size());
    if (version >= 5) {
        out.throttle.resize(out.cars.size());
        out.steer.resize(out.cars.size());
    }
    for (uint32_t i = 0; i < frames && r.ok; i++) {
        out.time[i] = r.get<float>();
        out.secs[i] = r.get<int32_t>();
        out.ball[i] = r.body();
        for (uint32_t k = 0; k < players; k++) {
            const size_t idx = static_cast<size_t>(i) * players + k;
            out.cars[idx] = r.body();
            out.boost[idx] = r.get<uint8_t>();
            out.boosting[idx] = r.get<uint8_t>();
            if (version >= 4) out.moves[idx] = r.get<uint8_t>();
            if (version >= 5) {
                out.throttle[idx] = r.get<uint8_t>();
                out.steer[idx] = r.get<uint8_t>();
            }
        }
    }
    const uint32_t goals = r.get<uint32_t>();
    for (uint32_t g = 0; g < goals && r.ok; g++) {
        const int f = static_cast<int>(r.get<uint32_t>());
        out.goals.push_back({f, r.get<uint8_t>()});
    }
    const uint32_t saves = r.get<uint32_t>();
    for (uint32_t s = 0; s < saves && r.ok; s++) {
        const int f = static_cast<int>(r.get<uint32_t>());
        out.saves.push_back({f, r.get<uint8_t>()});
    }
    out.looks.resize(players);
    for (uint32_t k = 0; k < players && version >= 2 && r.ok; k++) {
        Look& look = out.looks[k];
        look.known = r.get<uint8_t>() != 0;
        if (!look.known) continue;
        const uint8_t slots = r.get<uint8_t>();
        for (uint8_t i = 0; i < slots && r.ok; i++) {
            const int slot = r.get<uint8_t>();
            const int product = static_cast<int>(r.get<uint32_t>());
            const int paint = r.get<int32_t>();
            look.slots.push_back({slot, product, paint});
        }
        look.teamColor = r.get<uint8_t>();
        look.accentColor = r.get<uint8_t>();
        look.teamFinish = static_cast<int>(r.get<uint32_t>());
        look.accentFinish = static_cast<int>(r.get<uint32_t>());
    }
    out.cams.resize(players);
    if (version >= 3 && r.ok) out.ballArch = r.str();
    for (uint32_t k = 0; k < players && version >= 3 && r.ok; k++) {
        PlayerCam& cam = out.cams[k];
        cam.known = r.get<uint8_t>() != 0;
        if (!cam.known) continue;
        ProfileCameraSettings& c = cam.settings;
        for (float* v : {&c.FOV, &c.Height, &c.Pitch, &c.Distance, &c.Stiffness, &c.SwivelSpeed, &c.TransitionSpeed}) *v = r.get<float>();
        const uint32_t changes = r.get<uint32_t>();
        for (uint32_t i = 0; i < changes && r.ok; i++) {
            const float time = r.get<float>();
            cam.ballCam.push_back({time, r.get<uint8_t>() != 0});
        }
    }
    // version 5: how the match goes (each frame's phase and kickoff countdown), the score it starts from, who scored,
    // the demolitions
    out.scorers.assign(out.goals.size(), {-1, -1});
    if (version >= 5 && r.ok) {
        out.phase.resize(frames);
        out.countdown.resize(frames);
        for (uint32_t i = 0; i < frames && r.ok; i++) {
            out.phase[i] = std::min<uint8_t>(r.get<uint8_t>(), PHASE_GOAL);
            out.countdown[i] = r.get<uint8_t>();
        }
        for (int& s : out.baseScore) s = r.get<int32_t>();
        auto player = [players](uint8_t k) { return k < players ? static_cast<int>(k) : -1; };
        for (auto& [scorer, assist] : out.scorers) {
            scorer = player(r.get<uint8_t>());
            assist = player(r.get<uint8_t>());
        }
        const uint32_t demos = r.get<uint32_t>();
        for (uint32_t d = 0; d < demos && r.ok; d++) {
            const int frame = static_cast<int>(r.get<uint32_t>());
            const int victim = player(r.get<uint8_t>());
            r.get<uint8_t>();   // who demolished him
            if (victim >= 0 && frame < static_cast<int>(frames)) out.demos.push_back({frame, victim});
        }
        out.demosKnown = r.ok;
        if (version >= 6 && r.ok) {   // the boost pads
            const uint32_t events = r.get<uint32_t>();
            for (uint32_t e = 0; e < events && r.ok; e++) {
                const float x = r.get<float>();
                const float y = r.get<float>();
                const float time = r.get<float>();
                out.pads.push_back({x, y, time, r.get<uint8_t>() != 0});
            }
        }
        // Each goal's phase ends with a jump to the kickoff: at its largest gap (the game showed the goal again there,
        // or not at all if the players skipped it; an earlier smaller gap is lag)
        auto gap = [&out](uint32_t i) { return out.time[i + 1] - out.time[i]; };
        for (uint32_t a = 0; a < frames; a++) {
            if (out.phase[a] != PHASE_GOAL) continue;
            uint32_t b = a;   // the goal's phase: frames a to b
            while (b + 1 < frames && out.phase[b + 1] == PHASE_GOAL) b++;
            if (b + 1 < frames && out.phase[b + 1] == PHASE_COUNTDOWN) {   // a kickoff follows (not the end of the match)
                uint32_t cut = a;
                for (uint32_t i = a; i <= b; i++)
                    if (gap(i) > gap(cut)) cut = i;
                out.cuts.push_back(static_cast<int>(cut));
            }
            a = b;
        }
    }
    // overtime: the clock reaches 0, then counts up again
    for (uint32_t i = 0; i < frames; i++) {
        if (out.secs[i] != 0) continue;
        for (uint32_t j = i; j < frames; j++)
            if (out.secs[j] > 0) { out.otFrame = static_cast<int>(i); break; }
        break;
    }
    return r.ok;
}

void ReplayLab::OpenReplay(const std::string& id, int startFrame, bool simulate, const std::string& followed) {
    {
        std::lock_guard<std::recursive_mutex> lock(mtx);
        if (loading || !parseReplay) return;
        loading = true;
    }
    const std::string path = (std::filesystem::path(DemosFolder()) / (id + ".replay")).u8string();
    const std::string me = *opt.player;
    Status("Lecture du replay...", 20.f);
    RunWorker(openWorker, [this, id, path, startFrame, simulate, followed, me] {
        unsigned char* p = nullptr;
        size_t n = 0;
        char err[512] = {0};
        auto data = std::make_shared<ReplayData>();
        const bool ok = parseReplay(path.c_str(), &p, &n, err, sizeof err) == 0 && p && Decode(p, n, *data);
        if (ok) SmoothPaths(*data);
        if (p) freeBlob(p, n);
        data->id = id;
        const std::string error = err;
        // picked up by the game's next frame (TakeOpened), not queued to the game: nothing of the plugin runs once it is
        // unloaded
        std::lock_guard<std::recursive_mutex> handOff(mtx);
        opened = [this, data, ok, error, startFrame, simulate, followed, me] {
            loading = false;
            if (!ok) {
                Status("Replay illisible: " + error, 15.f);
                return;
            }
            replay = data;
            original = data;
            scenario = simSource = nullptr;
            branches.clear();   // they were another replay's
            branchCount = 0;
            CancelGoalReplay();
            mode = MODE_IDLE;
            carsReady = false;
            botSlot.clear();
            roles.assign(replay->names.size(), ROLE_GHOST);
            int toi = -1;
            for (int k = 0; k < replay->Players(); k++)
                if (replay->names[k] == followed) toi = k;
            for (int k = 0; k < replay->Players() && toi < 0; k++)
                if (replay->names[k] == me) toi = k;
            // spectator by default: your car rides along with this player; pick "Toi" in the window to drive
            camTarget = toi < 0 ? 0 : toi;
            const float t = replay->time[std::min(std::max(startFrame, 0), replay->Frames() - 1)];
            Status("Replay charge: " + replay->map + ", " + std::to_string(replay->Players()) + " joueurs.");
            EnterFreeplay([this, t, simulate] {
                SetupCars([this, t, simulate] {
                    StartPlayback(t, false);
                    if (simulate) Fork();
                }, simulate);
            });
        };
    });
}

void ReplayLab::TakeOpened() {
    std::lock_guard<std::recursive_mutex> lock(mtx);
    if (!opened) return;
    const std::function<void()> then = std::move(opened);
    opened = nullptr;
    then();
}

void ReplayLab::OpenFromViewer() {
    ReplayServerWrapper rs = gameWrapper->GetGameEventAsReplay();
    if (rs.IsNull() || rs.GetReplay().IsNull()) {
        Status("Replay pas encore pret, reessaie dans une seconde.");
        return;
    }
    const std::string id = HexId(rs.GetReplay().GetId().ToString());
    std::string followed;
    ActorWrapper target = rs.GetViewTarget();
    ArrayWrapper<CarWrapper> cars = rs.GetCars();
    for (int i = 0; i < cars.Count(); i++) {
        CarWrapper car = cars.Get(i);
        if (!car.IsNull() && !target.IsNull() && Same(target, car)) followed = NameOf(car);
    }
    if (!std::filesystem::exists(std::filesystem::path(DemosFolder()) / (id + ".replay"))) {
        Status("Fichier du replay introuvable dans Demos (" + id + ").", 10.f);
        return;
    }
    OpenReplay(id, rs.GetCurrentReplayFrame(), true, followed);
}
