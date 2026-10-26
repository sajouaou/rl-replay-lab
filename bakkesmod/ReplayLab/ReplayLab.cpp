#include "ReplayLab.h"
#include "LabMath.h"
#include "Palettes.h"

#include "bakkesmod/wrappers/Engine/EnumWrapper.h"
#include "bakkesmod/wrappers/Engine/WorldInfoWrapper.h"
#include "bakkesmod/wrappers/GameObject/CameraSettingsActorWrapper.h"
#include "bakkesmod/wrappers/GameObject/CarComponent/PrimitiveComponentWrapper.h"
#include "bakkesmod/core/bot_loadout_structs.h"
#include "imgui/imgui.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <sstream>
#include <thread>

#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

BAKKESMOD_PLUGIN(ReplayLab, "Replay Lab - rejouer un replay et reprendre la main", "3.4", PLUGINTYPE_FREEPLAY | PLUGINTYPE_REPLAY)

using namespace lab;

namespace {
constexpr float RELEASE_DIST = 320.f;     // the ball stops following the replay when a changed car gets this close
constexpr float PARK_Z = -600.f;          // under the field floor (z = 0)
constexpr float GOAL_Y = 5120.f + 93.f;   // goal line + ball radius: the whole ball is in
constexpr float TICK = 1.f / 120.f;       // one physics step of the game
constexpr double SMOOTH = 20.0;          // weight of the recorded velocities against the recorded positions
constexpr float TELEPORT = 250.f;        // this far from where the velocity leads: a teleport, not smoothed across
constexpr int OCTANE = 23;
// Octane hitbox: half extents and offset from the car's center of mass (game units)
constexpr float HALF_L = 59.f, HALF_W = 42.f, HALF_H = 18.f, OFF_F = 13.88f, OFF_U = 20.75f;
const char* ROLE_LABELS[ROLE_COUNT] = {"Toi (tu la conduis)", "Fantome (refait le vrai trajet)", "Retiree",
                                       "Bot Psyonix", "Libre (sans pilote)", "Nexto (IA RLGym)"};
// each mode's freeplay: the game class loads the mode's package, and its freeplay has the mode's ball
// (soccar keeps the training freeplay Replay Lab was made with)
const char* KIND_CLASS[KIND_COUNT] = {"TAGame.GameInfo_Tutorial_TA", "TAGame.GameInfo_Basketball_TA", "TAGame.GameInfo_Hockey_TA",
                                      "TAGame.GameInfo_Breakout_TA"};
const char* KIND_LABELS[KIND_COUNT] = {"Soccar", "Hoops", "Snow Day", "Dropshot"};

// the mode of a replay, from the ball its game used (Rumble, Heatseeker, mutator balls... play as soccar)
int KindOfBall(const std::string& arch) {
    if (arch.find("BasketBall") != std::string::npos) return KIND_HOOPS;
    if (arch.find("Puck") != std::string::npos) return KIND_SNOWDAY;
    if (arch.find("Breakout") != std::string::npos) return KIND_DROPSHOT;
    return KIND_SOCCAR;
}

// the mode a freeplay plays, from its ball (radius measured in the game: 95.5 soccar, 98.1 Hoops, 29 the puck, 102 Dropshot)
int KindOfRadius(float r) {
    if (r < 60.f) return KIND_SNOWDAY;
    if (r > 100.f) return KIND_DROPSHOT;
    if (r > 97.f) return KIND_HOOPS;
    return KIND_SOCCAR;
}

// the ball (its center) has scored, by the rules of the mode
bool Scored(const Vector& p, int kind) {
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
const Arena ARENAS[KIND_COUNT] = {{4096.f, 5120.f, 2044.f}, {2966.67f, 3581.f, 1820.f}, {4096.f, 5120.f, 2044.f}, {0.f, 0.f, 0.f}};

// a car with its wheels on the floor, a wall or the ceiling: where it gets its jump and flip back
bool OnSurface(const RBody& b, int kind) {
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
bool WheelsOnBall(const RBody& car, const RBody& ball) {
    if (!car.valid || !ball.valid) return false;
    const Vector d = Sub(ball.pos, car.pos);
    const float l = Len(d);
    return l > 1.f && l < 190.f && Dot(AxesOf(ToRotator(car.quat)).u, d) < -0.5f * l;
}

const float SPEEDS[] = {0.25f, 0.5f, 1.f, 2.f};
const char* SPEED_LABELS[] = {"x0.25", "x0.5", "x1", "x2"};

void Nlerp(const float a[4], const float b[4], float k, float out[4]) {
    const float sign = (a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3]) < 0 ? -1.f : 1.f;
    float n = 0;
    for (int i = 0; i < 4; i++) {
        out[i] = a[i] * (1 - k) + sign * b[i] * k;
        n += out[i] * out[i];
    }
    n = std::sqrt(std::max(n, 1e-12f));
    for (int i = 0; i < 4; i++) out[i] /= n;
}

void Color(CanvasWrapper& c, int r, int g, int b, int a) {
    c.SetColor(static_cast<char>(r), static_cast<char>(g), static_cast<char>(b), static_cast<char>(a));
}

// a swatch (0-255) as the colour a car's paint takes: as it is (0-1); rlab_gamma 2.2 would make it linear, which turned
// orange red and purple black
LinearColor Paint(const unsigned char c[3], float gamma) {
    auto f = [gamma](unsigned char v) { return std::pow(v / 255.f, gamma); };
    return LinearColor(f(c[0]), f(c[1]), f(c[2]), 1.f);
}

// any stick, trigger or button past a small dead zone
bool Moves(const ControllerInput& in) {
    const float dead = 0.2f;
    return std::fabs(in.Throttle) > dead || std::fabs(in.Steer) > dead || std::fabs(in.Pitch) > dead || std::fabs(in.Yaw) > dead ||
           std::fabs(in.Roll) > dead || in.Jump || in.ActivateBoost || in.HoldingBoost || in.Handbrake;
}

void ZeroInput(ControllerInput* in) {
    in->Throttle = in->Steer = in->Pitch = in->Yaw = in->Roll = in->DodgeForward = in->DodgeStrafe = 0.f;
    in->Handbrake = in->Jump = in->ActivateBoost = in->HoldingBoost = 0;
}


bool Same(ObjectWrapper a, ObjectWrapper b) { return a.memory_address == b.memory_address; }

std::string HexId(const std::string& raw) {
    std::string out;
    for (char c : raw)
        if (std::isxdigit(static_cast<unsigned char>(c))) out += static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return out;
}

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

/* ================================================================== setup */

void ReplayLab::onLoad() {
    cvarManager->registerCvar("rlab_player", "Anxoide", "Ton pseudo: ta voiture par defaut dans les replays");
    cvarManager->registerCvar("rlab_speed", "1.0", "Vitesse du jeu pendant la simulation (0.5 = ralenti)", true, true, 0.1f, true, 1.f);
    cvarManager->registerCvar("rlab_ghosts", "1", "Dessiner le vrai replay en fil de fer pendant la simulation", true, true, 0.f, true, 1.f);
    cvarManager->registerCvar("rlab_mirror", "0", "Voiture orange: tourner la scene pour defendre le but du bas", true, true, 0.f, true, 1.f);
    // version 3.3: each car plays in its real team, so the scene no longer needs turning around by default
    CVarWrapper teamsV33 = cvarManager->registerCvar("rlab_teams_v33", "0", "", false);
    if (!teamsV33.getBoolValue()) {
        cvarManager->getCvar("rlab_mirror").setValue(0);
        teamsV33.setValue(1);
    }
    cvarManager->registerCvar("rlab_sync", "1", "Le ballon suit le replay tant qu'aucune voiture changee ne s'approche", true, true, 0.f, true, 1.f);
    cvarManager->registerCvar("rlab_debug", "0", "Ecrire dans la console ou chaque voiture devrait etre et ou elle est (2: chaque pas)", true, true, 0.f, true, 2.f, false);

    // the same command names as before, so existing key binds keep working
    cvarManager->registerNotifier("rlab_capture", [this](std::vector<std::string>) { TogglePlay(); },
                                  "Lecture / pause du replay (en simulation: retour au replay)", PERMISSION_ALL);
    cvarManager->registerNotifier("rlab_play", [this](std::vector<std::string>) { OnSimulateKey(); },
                                  "Simuler d'ici", PERMISSION_ALL);
    cvarManager->registerNotifier("rlab_retry", [this](std::vector<std::string>) {
        std::lock_guard<std::recursive_mutex> lock(mtx);
        if (!replay || mode == MODE_IDLE) return;
        SetupCars([this] { rt = forkT; Fork(); }, true);
    }, "Recommencer la simulation", PERMISSION_ALL);
    cvarManager->registerNotifier("rlab_camera", [this](std::vector<std::string>) { NextView(); },
                                  "Regarder depuis la voiture du joueur suivant", PERMISSION_ALL);
    cvarManager->registerNotifier("rlab_open", [this](std::vector<std::string> args) {
        if (args.size() < 2) return;
        const int frame = args.size() > 2 ? std::atoi(args[2].c_str()) : 0;
        const bool sim = args.size() > 3 && args[3] == "1";
        OpenReplay(HexId(args[1]), frame, sim, "");
    }, "rlab_open <id du replay> [image] [1 = simuler tout de suite]", PERMISSION_ALL);
    cvarManager->registerNotifier("rlab_role", [this](std::vector<std::string> args) {
        std::lock_guard<std::recursive_mutex> lock(mtx);
        if (!replay || args.size() < 3) return;
        const int k = std::atoi(args[1].c_str()) - 1, role = std::atoi(args[2].c_str());
        if (k < 0 || k >= replay->Players() || role < 0 || role >= ROLE_COUNT) return;
        const bool sim = mode == MODE_SIM;
        if (SetRole(k, role)) SetupCars([this, sim] { if (!sim) StartPlayback(rt, playing); }, sim);
        Status(replay->names[k] + ": " + ROLE_LABELS[role] + " (prochaine simulation / F11)");
    }, "rlab_role <joueur 1..N> <0 toi, 1 fantome, 2 retiree, 3 bot Psyonix, 4 libre, 5 Nexto>", PERMISSION_ALL);
    // a title and a caption over the game, kept in cinema mode: to make a video ("first line | second line", nothing: off)
    auto text = [](const std::vector<std::string>& args, std::string out[2]) {
        std::string all;
        for (size_t i = 1; i < args.size(); i++) all += (i > 1 ? " " : "") + args[i];
        const size_t bar = all.find(" | ");
        out[0] = all.substr(0, bar);
        out[1] = bar == std::string::npos ? "" : all.substr(bar + 3);
    };
    cvarManager->registerNotifier("rlab_vitesse", [this](std::vector<std::string> args) {
        std::lock_guard<std::recursive_mutex> lock(mtx);
        if (args.size() > 1) playSpeed = std::min(std::max(static_cast<float>(std::atof(args[1].c_str())), 0.1f), 4.f);
    }, "rlab_vitesse <0.25 .. 2>: vitesse de lecture du replay (ralenti)", PERMISSION_ALL);
    cvarManager->registerNotifier("rlab_hud", [this](std::vector<std::string>) {
        gameWrapper->Execute([](GameWrapper* gw) { gw->ExecuteUnrealCommand("ShowHUD"); });
    }, "rlab_hud: afficher / cacher l'interface du jeu (jauge de boost, aides), pour filmer", PERMISSION_ALL);
    cvarManager->registerNotifier("rlab_titre", [this, text](std::vector<std::string> args) {
        std::lock_guard<std::recursive_mutex> lock(mtx);
        text(args, title);
    }, "rlab_titre <titre | sous-titre>: grand titre a l'ecran (sans texte: l'enlever)", PERMISSION_ALL);
    cvarManager->registerNotifier("rlab_legende", [this, text](std::vector<std::string> args) {
        std::lock_guard<std::recursive_mutex> lock(mtx);
        text(args, caption);
    }, "rlab_legende <texte | detail>: legende en bas de l'ecran (sans texte: l'enlever)", PERMISSION_ALL);
    cvarManager->registerNotifier("rlab_cam_mode", [this](std::vector<std::string> args) {
        if (args.size() > 1) camMode = (std::atoi(args[1].c_str()) + CAM_COUNT - 1) % CAM_COUNT;   // that one: the one before, then next
        NextCamMode();
    },
                                  "Camera suivante: jeu, joueur, libre, automatique (F7)", PERMISSION_ALL);
    cvarManager->registerNotifier("rlab_ballcam", [this](std::vector<std::string>) { ToggleBallCam(); },
                                  "Camera ballon (F8)", PERMISSION_ALL);
    cvarManager->registerNotifier("rlab_revoir", [this](std::vector<std::string>) { WatchScenario(); },
                                  "Revoir la derniere simulation comme un replay", PERMISSION_ALL);
    cvarManager->registerNotifier("rlab_origine", [this](std::vector<std::string>) { BackToOriginal(); },
                                  "Revenir au replay d'origine", PERMISSION_ALL);
    cvarManager->registerCvar("rlab_gamma", "1", "Couleurs des voitures: 1 telles quelles, 2.2 lineaires", true, true, 0.3f, true, 3.f);
    cvarManager->registerCvar("rlab_attendre", "1", "Comme l'entrainement: quand tu conduis, la simulation attend que tu bouges", true, true, 0.f, true, 1.f);
    cvarManager->registerCvar("rlab_cam_joueur", "1", "Vue d'un joueur: ses reglages de camera et sa camera ballon, comme dans le replay", true, true, 0.f, true, 1.f);
    cvarManager->registerCvar("rlab_noms", "1", "Nom de chaque joueur au-dessus de sa voiture, a la couleur de son equipe (reste en mode cinema)",
                              true, true, 0.f, true, 1.f);
    cvarManager->registerCvar("rlab_tableau", "1", "Score et horloge en haut de l'ecran, comme en match (reste en mode cinema)", true, true, 0.f, true, 1.f);
    cvarManager->registerCvar("rlab_cinema", "0", "Mode cinema: aucun texte ni dessin de Replay Lab a l'ecran (pour filmer)", true, true, 0.f, true, 1.f);
    cvarManager->registerNotifier("rlab_photo", [this](std::vector<std::string>) { gameWrapper->ExecuteUnrealCommand("shot"); },
                                  "Capture d'ecran du jeu (dossier Screenshots du jeu)", PERMISSION_ALL);
    cvarManager->registerNotifier("rlab_etat", [this](std::vector<std::string>) {
        std::lock_guard<std::recursive_mutex> lock(mtx);
        static const char* MODES[] = {"aucun replay", "lecture", "simulation"};
        char buf[220];
        std::snprintf(buf, sizeof buf, "Replay Lab etat: %s, t=%.2f, %s, fenetre=%d, jeu en pause=%d, ta voiture=%s, entrainement libre=%d, plots connus=%d",
                      MODES[mode], replay ? Now() - Start() : 0.f, playing ? "lecture" : "arret", windowOpen ? 1 : 0, gameWrapper->IsPaused() ? 1 : 0,
                      gameWrapper->GetLocalCar().IsNull() ? "absente" : "presente", gameWrapper->IsInFreeplay() ? 1 : 0,
                      static_cast<int>(pads.size()));
        cvarManager->log(buf);
        ServerWrapper srv = gameWrapper->GetGameEventAsServer();
        if (!srv.IsNull()) {   // what is in the game: player infos, cars, controllers
            ArrayWrapper<PriWrapper> pris = srv.GetPRIs();
            ArrayWrapper<CarWrapper> cars = srv.GetCars();
            ArrayWrapper<ControllerWrapper> players = srv.GetPlayers();
            std::string who;
            for (int i = 0; i < pris.Count(); i++)
                if (!pris.Get(i).IsNull())
                    who += " " + pris.Get(i).GetPlayerName().ToString() + (pris.Get(i).GetbBot() ? "" : "(toi)") +
                           (pris.Get(i).GetCar().IsNull() ? "[sans voiture]" : "");
            cvarManager->log("Replay Lab etat: " + std::to_string(pris.Count()) + " joueurs, " + std::to_string(cars.Count()) + " voitures, " +
                             std::to_string(players.Count()) + " controleurs:" + who);
            std::string list = "Replay Lab etat: voitures:";
            for (int i = 0; i < cars.Count(); i++) {
                CarWrapper car = cars.Get(i);
                if (car.IsNull()) continue;
                char one[120];
                std::snprintf(one, sizeof one, " [%s z=%.0f%s%s]", NameOf(car).c_str(), car.GetLocation().Z, car.GetbHidden() ? " cachee" : "",
                              car.GetPRI().IsNull() ? " sans-joueur" : "");
                list += one;
            }
            cvarManager->log(list);
            BallWrapper ball = srv.GetBall();
            char what[260];
            if (!ball.IsNull()) {
                std::snprintf(what, sizeof what, "Replay Lab etat: ballon rayon=%.1f visuel=%.1f taille=%.2f rebond=%.2f gravite=%.2f, %d ballon(s), stade %s",
                              ball.GetRadius(), ball.GetVisualRadius(), ball.GetReplicatedBallScale(), ball.GetReplicatedWorldBounceScale(),
                              ball.GetReplicatedBallGravityScale(), srv.GetGameBalls().Count(), gameWrapper->GetCurrentMap().c_str());
                cvarManager->log(what);
            }
            ArrayWrapper<GoalWrapper> goals = srv.GetGoals();
            for (int i = 0; i < goals.Count(); i++) {
                GoalWrapper g = goals.Get(i);
                if (g.IsNull()) continue;
                const Vector c = g.GetWorldCenter(), e = g.GetWorldExtent(), l = g.GetLocation(), d = g.GetDirection();
                std::snprintf(what, sizeof what, "Replay Lab etat: but equipe %d centre (%.0f,%.0f,%.0f) demi-taille (%.0f,%.0f,%.0f) lieu (%.0f,%.0f,%.0f) dir (%.2f,%.2f,%.2f)",
                              g.GetTeamNum(), c.X, c.Y, c.Z, e.X, e.Y, e.Z, l.X, l.Y, l.Z, d.X, d.Y, d.Z);
                cvarManager->log(what);
            }
        }
        if (mode == MODE_SIM) {
            int sc[2];
            ScoreAt(Now(), sc);
            cvarManager->log("Replay Lab etat: horloge " + SimClockText() + " (" + std::to_string(SimClock()) + " s), match " + std::to_string(match) +
                             ", score " + std::to_string(sc[0]) + "-" + std::to_string(sc[1]));
        }
        cvarManager->log("Replay Lab etat: croix " + std::to_string(padKey[0]) + "/" + std::to_string(padKey[1]) + "/" +
                         std::to_string(padKey[2]) + "/" + std::to_string(padKey[3]) + ", simuler " + padSimulate + "=" +
                         std::to_string(padKey[4]) + ", camera ballon octet " + std::to_string(ballCamByte) +
                         " bit " + std::to_string(ballCamBit) + ", suivie " + (BallCamOn() ? "oui" : "non") + ", ecrite " + std::to_string(ballCamWrites) + " fois");
        std::string binds = "Replay Lab etat: liaisons BakkesMod restantes:";
        for (const char* key : {"F3", "F4", "F5", "F9", "F11"}) binds += std::string(" ") + key + "=[" + cvarManager->getBindStringForKey(key) + "]";
        cvarManager->log(binds);
        CameraWrapper cam = gameWrapper->GetCamera();
        if (!cam.IsNull() && replay) {
            const ViewTarget vt = cam.GetViewTarget();
            ActorWrapper followed(reinterpret_cast<uintptr_t>(vt.Target));
            CarWrapper dolly = Dolly();
            char view[260];
            std::snprintf(view, sizeof view, "Replay Lab etat: %s%s, %s, camera en (%.0f,%.0f,%.0f), a %.0f de ce qu'elle suit (%s)%s",
                          replay->scenario ? "SCENARIO " : "", CamText().c_str(), cam.GetCameraState().c_str(),
                          cam.GetLocation().X, cam.GetLocation().Y, cam.GetLocation().Z, followed.IsNull() ? -1.f : Len(Sub(followed.GetLocation(), cam.GetLocation())),
                          followed.IsNull() ? "rien" : (CarWrapper(followed.memory_address).GetPRI().IsNull() ? "?" : NameOf(CarWrapper(followed.memory_address)).c_str()),
                          dolly.IsNull() ? "" : (std::string(", voiture-camera cachee=") + std::to_string(dolly.GetbHidden()) + " collisions=" +
                                                 std::to_string(dolly.GetbCollideActors())).c_str());
            cvarManager->log(view);
            std::snprintf(view, sizeof view, "Replay Lab etat: mode %s (ballon du replay %s)", KIND_LABELS[GameKind()], replay->ballArch.c_str());
            cvarManager->log(view);
            auto settingsText = [](const ProfileCameraSettings& s) {
                char b[100];
                std::snprintf(b, sizeof b, "fov %.0f h %.0f d %.0f pivot %.1f", s.FOV, s.Height, s.Distance, s.SwivelSpeed);
                return std::string(b);
            };
            CameraSettingsActorWrapper csa = cam.GetCameraSettingsActor();
            std::string more = "Replay Lab etat: fov reel " + std::to_string(cam.GetFOV()) + ", acteur reglages camera: " +
                               (csa.IsNull() ? "aucun" : (csa.GetPri().IsNull() ? "?" : csa.GetPri().GetPlayerName().ToString()) + " " + settingsText(csa.GetProfileSettings()));
            if (!followed.IsNull() && !srv.IsNull() && !srv.GetBall().IsNull()) {
                const Vector toBall = Sub(srv.GetBall().GetLocation(), cam.GetLocation());
                char yaw[160];
                std::snprintf(yaw, sizeof yaw, ", lacet camera %.0f voiture %.0f vers le ballon %.0f",
                              cam.GetRotation().Yaw / 182.044f, followed.GetRotation().Yaw / 182.044f,
                              std::atan2(toBall.Y, toBall.X) * 57.2958f);
                more += yaw;
            }
            if (!followed.IsNull()) {
                PriWrapper fp = CarWrapper(followed.memory_address).GetPRI();
                if (!fp.IsNull())
                    more += ", joueur suivi " + fp.GetPlayerName().ToString() + ": " + settingsText(fp.GetCameraSettings()) + " ballcam " +
                            std::to_string(fp.GetbUsingSecondaryCamera()) + " acteur " + (fp.GetPersistentCamera().IsNull() ? "aucun" : "oui");
            }
            CarWrapper me = gameWrapper->GetLocalCar();
            if (!me.IsNull())
                more += ", ta voiture: sol " + std::to_string(me.GetbOnGround()) + " saute " + std::to_string(me.GetbJumped()) + " double " +
                        std::to_string(me.GetbDoubleJumped()) + " flip " + std::to_string(me.HasFlip());
            if (!me.IsNull() && !me.GetPRI().IsNull())
                more += ", toi: " + settingsText(me.GetPRI().GetCameraSettings()) + " ballcam " + std::to_string(me.GetPRI().GetbUsingSecondaryCamera()) +
                        " acteur " + (me.GetPRI().GetPersistentCamera().IsNull() ? "aucun" : "oui");
            cvarManager->log(more);
        }
    }, "rlab_etat: ecrire l'etat de Replay Lab dans la console", PERMISSION_ALL);
    cvarManager->registerCvar("rlab_manette", "1", "Manette: croix haut lecture/pause, gauche camera, droite joueur, bas statut; "
                              "Back: simuler d'ici; reset: recommencer", true, true, 0.f, true, 1.f);
    cvarManager->registerCvar("rlab_manette_simuler", "XboxTypeS_Back",
                              "Bouton de manette qui lance la simulation (nom de touche du jeu, ex. XboxTypeS_LeftThumbstick)");
    cvarManager->registerNotifier("rlab_statut", [this](std::vector<std::string>) { NextRole(); },
                                  "Statut suivant (fantome, toi, bot...) du joueur que la camera montre", PERMISSION_ALL);
    cvarManager->registerNotifier("rlab_demolir", [this](std::vector<std::string> args) {
        if (args.size() >= 2) DemolishPlayer(std::atoi(args[1].c_str()) - 1);
    }, "rlab_demolir <joueur 1..N>: pendant une simulation, demolir ce joueur maintenant", PERMISSION_ALL);

    // the plugin reads its keys itself (PollKeys): BakkesMod binds to its commands would fire them twice
    for (const char* key : {"F3", "F4", "F5", "F7", "F8", "F9", "F10", "F11"}) {
        const std::string bound = cvarManager->getBindStringForKey(key);
        if (bound.find("rlab_") != std::string::npos || bound.find("togglemenu replaylab") != std::string::npos)
            cvarManager->removeBind(key);
    }

    // freeplay goals are switched off while the ball follows the replay; keep the player's settings to restore them
    CVarWrapper goal = cvarManager->getCvar("sv_freeplay_enablegoal");
    CVarWrapper goalBm = cvarManager->getCvar("sv_freeplay_enablegoal_bakkesmod");
    origGoal = goal.IsNull() ? "" : goal.getStringValue();
    origGoalBm = goalBm.IsNull() ? "" : goalBm.getStringValue();

    // cars driven by the replay overlap where real players touched: no demolitions while the replay plays,
    // the normal rule (the other team) in a simulation
    EnumWrapper targets = EnumWrapper::GetDemolishTargets();
    if (!targets.IsNull()) {
        std::string known;
        for (const auto& [value, label] : targets.GetPossibleValues()) {
            known += " " + label + "=" + std::to_string(value);
            std::string lower = label;
            std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            if (lower.find("none") != std::string::npos) noDemo = value;
            if (lower.find("otherteam") != std::string::npos) normalDemo = value;
        }
        cvarManager->log("Replay Lab: demolitions" + known + " -> replay " + std::to_string(noDemo) + ", simulation " +
                         std::to_string(normalDemo));
    }

    gameWrapper->HookEvent("Function TAGame.GameEvent_TA.SpawnCar", [this](std::string) { OnSpawnCar(); });
    gameWrapper->HookEvent("Function TAGame.GameEvent_TA.PlayerResetTraining", [this](std::string) { OnGameReset(); });
    gameWrapper->HookEvent("Function Engine.GameViewportClient.Tick", [this](std::string) {
        PollKeys();
        OnFrame();
    });
    gameWrapper->HookEventWithCaller<CarWrapper>("Function TAGame.Car_TA.SetVehicleInput",
        [this](CarWrapper car, void* params, std::string) { OnCarInput(car, params); });
    for (const char* demolished : {"Function TAGame.Car_TA.Demolish", "Function TAGame.Car_TA.EventDemolished"})
        gameWrapper->HookEventWithCaller<CarWrapper>(demolished, [this](CarWrapper car, void*, std::string) { OnDemolished(car); });
    gameWrapper->RegisterDrawable([this](CanvasWrapper canvas) { DrawHud(canvas); });
    // the game's camera follows the car of the camera mode: its target, changed just before it works out its view
    gameWrapper->HookEventWithCaller<ActorWrapper>("Function TAGame.CameraState_Car_TA.UpdatePOV",
        [this](ActorWrapper, void*, std::string) { AimCamera(); });

    std::string nextoError;
    if (!nextoModel.Load(std::filesystem::path(gameWrapper->GetBakkesModPathW()) / L"plugins" / L"replaylab" / L"nexto.bin", nextoError))
        cvarManager->log("Replay Lab: Nexto indisponible (" + nextoError + "), ses voitures seront des bots Psyonix");
    gameWrapper->HookEventWithCaller<ActorWrapper>("Function TAGame.VehiclePickup_TA.OnPickUp",
        [this](ActorWrapper pad, void*, std::string) { OnPadPickedUp(pad); });
    if (LoadReader()) {
        RefreshList();
        cvarManager->log("Replay Lab 3.4 charge: F3 fenetre, F4 vue, F9 lecture/pause, F5/F10 simuler d'ici, F11 recommencer; manette: "
                         "croix haut pause, gauche camera, droite joueur, bas statut, Back simuler d'ici, reset recommencer");
    }
}

// The F keys, read straight from Windows (Wine) while the game window is in front, not through the game's own input:
// after an Alt+Tab under Proton the game can stop seeing the keyboard (or believe Alt is still held).
void ReplayLab::PollKeys() {
    static const struct {
        int vk;
        const char* command;
    } KEYS[] = {{VK_F3, "togglemenu replaylab"}, {VK_F4, "rlab_camera"}, {VK_F5, "rlab_play"},   {VK_F7, "rlab_cam_mode"},
                {VK_F8, "rlab_ballcam"},         {VK_F9, "rlab_capture"}, {VK_F10, "rlab_play"}, {VK_F11, "rlab_retry"}};
    static_assert(sizeof(KEYS) / sizeof(KEYS[0]) == sizeof(keyDown) / sizeof(keyDown[0]), "one state per key");
    DWORD pid = 0;
    HWND front = GetForegroundWindow();
    if (front) GetWindowThreadProcessId(front, &pid);
    const bool ours = front && pid == GetCurrentProcessId();
    if (ours != gameInFront) {
        gameInFront = ours;
        cvarManager->log(ours ? "Replay Lab: jeu au premier plan" : "Replay Lab: jeu en arriere-plan");
    }
    for (size_t i = 0; i < sizeof(KEYS) / sizeof(KEYS[0]); i++) {
        const bool down = ours && (GetAsyncKeyState(KEYS[i].vk) & 0x8000) != 0;
        if (down && !keyDown[i]) cvarManager->executeCommand(KEYS[i].command);
        keyDown[i] = down;
    }
    // the controller's d-pad and simulate button, as the game sees them (any controller it knows goes by the Xbox names)
    static const char* PAD[] = {"XboxTypeS_DPad_Up", "XboxTypeS_DPad_Left", "XboxTypeS_DPad_Right", "XboxTypeS_DPad_Down"};
    static const char* PAD_COMMANDS[] = {"rlab_capture", "rlab_cam_mode", "rlab_camera", "rlab_statut", "rlab_play"};
    const std::string simulate = cvarManager->getCvar("rlab_manette_simuler").getStringValue();
    if (simulate != padSimulate) {
        padSimulate = simulate;
        padKey[4] = simulate.empty() ? -1 : gameWrapper->GetFNameIndexByString(simulate);
    }
    const bool pad = cvarManager->getCvar("rlab_manette").getBoolValue();
    for (int i = 0; i < 5; i++) {
        if (i < 4 && padKey[i] < 0) padKey[i] = gameWrapper->GetFNameIndexByString(PAD[i]);
        // simulate also works in the game's replay viewer (opens this replay there, like F5)
        const bool on = pad && (replay && mode != MODE_IDLE || (i == 4 && gameWrapper->IsInReplay()));
        const bool down = on && padKey[i] >= 0 && gameWrapper->IsKeyPressed(padKey[i]);
        if (down && !padDown[i]) cvarManager->executeCommand(PAD_COMMANDS[i]);
        padDown[i] = down;
    }
    if (roleNext >= 0 && std::chrono::steady_clock::now() >= roleAt) {
        std::lock_guard<std::recursive_mutex> lock(mtx);
        const int k = rolePlayer, role = roleNext;
        roleNext = -1;
        if (replay && k < replay->Players()) {
            const bool sim = mode == MODE_SIM;
            if (SetRole(k, role)) SetupCars([this, sim] { if (!sim) StartPlayback(rt, playing); }, sim);
            Status(replay->names[k] + ": " + ROLE_LABELS[role] + (role == ROLE_TOI ? "" : " (a la prochaine simulation: bouton reset)"), 5.f);
        }
    }
}

// D-pad down: the next role for the player the camera shows (you, if it shows your car), applied once you stop
// pressing, so going through the roles does not set up the cars at each one
void ReplayLab::NextRole() {
    std::lock_guard<std::recursive_mutex> lock(mtx);
    if (!replay || mode == MODE_IDLE) return;
    static const int ORDER[] = {ROLE_GHOST, ROLE_TOI, ROLE_BOT, ROLE_NEXTO, ROLE_FREE, ROLE_REMOVED};
    const int k = roleNext >= 0 ? rolePlayer : (ShownMode() == CAM_GAME && CarPlayer() >= 0 ? CarPlayer() : camTarget);
    if (k < 0 || k >= replay->Players()) return;
    const int from = roleNext >= 0 ? roleNext : roles[k];
    int at = 0;
    while (at < 6 && ORDER[at] != from) at++;
    rolePlayer = k;
    roleNext = ORDER[(at + 1) % 6];
    roleAt = std::chrono::steady_clock::now() + std::chrono::milliseconds(1200);
    Status("Statut de " + replay->names[k] + ": " + ROLE_LABELS[roleNext] + "   (croix bas: suivant)", 3.f);
}

// your controller's reset button (the game's training reset, whatever button it is on): restart, like F11
void ReplayLab::OnGameReset() {
    const auto now = std::chrono::steady_clock::now();
    if (!replay || mode == MODE_IDLE || !cvarManager->getCvar("rlab_manette").getBoolValue() ||
        std::chrono::duration<float>(now - lastReset).count() < 0.5f)
        return;
    lastReset = now;
    gameWrapper->Execute([this](GameWrapper*) { cvarManager->executeCommand("rlab_retry"); });
}

void ReplayLab::onUnload() {
    gameWrapper->UnhookEvent("Function Engine.GameViewportClient.Tick");
    gameWrapper->UnhookEvent("Function TAGame.Car_TA.SetVehicleInput");
    gameWrapper->UnhookEvent("Function TAGame.Car_TA.Demolish");
    gameWrapper->UnhookEvent("Function TAGame.Car_TA.EventDemolished");
    gameWrapper->UnhookEvent("Function TAGame.VehiclePickup_TA.OnPickUp");
    gameWrapper->UnhookEvent("Function TAGame.GameEvent_TA.SpawnCar");
    gameWrapper->UnhookEvent("Function TAGame.GameEvent_TA.PlayerResetTraining");
    if (gameWrapper->IsInFreeplay()) BallBack(gameWrapper->GetGameEventAsServer());   // not left hidden by a match's end
    gameWrapper->UnhookEvent("Function TAGame.CameraState_Car_TA.UpdatePOV");
    if (reader && !loading) {   // the next Replay Lab loads the installed reader (not while a replay is being read)
        FreeLibrary(static_cast<HMODULE>(reader));
        reader = nullptr;
    }
    if (!origGoalBm.empty()) cvarManager->executeCommand("sv_freeplay_enablegoal_bakkesmod " + origGoalBm, false);
    if (!origGoal.empty()) cvarManager->executeCommand("sv_freeplay_enablegoal " + origGoal, false);
}

void ReplayLab::SetImGuiContext(uintptr_t ctx) { ImGui::SetCurrentContext(reinterpret_cast<ImGuiContext*>(ctx)); }

void ReplayLab::Status(const std::string& text, float seconds) {
    std::lock_guard<std::recursive_mutex> lock(mtx);
    status = text;
    statusUntil = std::chrono::steady_clock::now() + std::chrono::milliseconds(static_cast<int>(seconds * 1000));
    cvarManager->log(text);
}

bool ReplayLab::LoadReader() {
    const std::filesystem::path dll = std::filesystem::path(gameWrapper->GetBakkesModPathW()) / L"plugins" / L"replaylab" / L"rlab_replay.dll";
    // a copy an earlier Replay Lab left loaded would be handed back instead of the installed one
    for (int i = 0; i < 8; i++) {
        HMODULE old = GetModuleHandleW(dll.wstring().c_str());
        if (!old) break;
        FreeLibrary(old);
    }
    HMODULE h = LoadLibraryW(dll.wstring().c_str());
    if (h) {
        parseReplay = reinterpret_cast<ParseFn>(GetProcAddress(h, "rlab_parse_replay"));
        replayHeader = reinterpret_cast<ParseFn>(GetProcAddress(h, "rlab_replay_header"));
        freeBlob = reinterpret_cast<FreeFn>(GetProcAddress(h, "rlab_free"));
        reader = h;
    }
    if (!parseReplay || !replayHeader || !freeBlob) {
        Status("Replay Lab: rlab_replay.dll introuvable dans plugins/replaylab (relance install_plugin.sh).", 30.f);
        return false;
    }
    return true;
}

std::wstring ReplayLab::DemosFolder() const {
    wchar_t buf[MAX_PATH] = {0};
    GetEnvironmentVariableW(L"USERPROFILE", buf, MAX_PATH);
    return std::wstring(buf) + L"\\Documents\\My Games\\Rocket League\\TAGame\\Demos";
}

/* ================================================================== replay files */

void ReplayLab::RefreshList() {
    {
        std::lock_guard<std::recursive_mutex> lock(mtx);
        if (listing || !replayHeader) return;
        listing = true;
    }
    const std::wstring folder = DemosFolder();
    const std::string me = cvarManager->getCvar("rlab_player").getStringValue();
    std::thread([this, folder, me] {
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
            entry.label = entry.date.substr(0, 10) + "  " + kv["TeamSize"] + "v" + kv["TeamSize"] + "  " + entry.map + "  " +
                          std::to_string(s0) + "-" + std::to_string(s1) + result;
            out.push_back(entry);
        }
        std::sort(out.begin(), out.end(), [](const ReplayEntry& a, const ReplayEntry& b) { return a.date > b.date; });
        std::lock_guard<std::recursive_mutex> lock(mtx);
        library = out;
        listing = false;
    }).detach();
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
    if (version < 1 || version > 4) return false;
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
    const std::string me = cvarManager->getCvar("rlab_player").getStringValue();
    Status("Lecture du replay...", 20.f);
    std::thread([this, id, path, startFrame, simulate, followed, me] {
        unsigned char* p = nullptr;
        size_t n = 0;
        char err[512] = {0};
        auto data = std::make_shared<ReplayData>();
        const bool ok = parseReplay(path.c_str(), &p, &n, err, sizeof err) == 0 && p && Decode(p, n, *data);
        if (ok) SmoothPaths(*data);
        if (p) freeBlob(p, n);
        data->id = id;
        const std::string error = err;
        gameWrapper->Execute([this, data, ok, error, startFrame, simulate, followed, me](GameWrapper*) {
            std::lock_guard<std::recursive_mutex> lock(mtx);
            loading = false;
            if (!ok) {
                Status("Replay illisible: " + error, 15.f);
                return;
            }
            replay = data;
            original = data;
            scenario = nullptr;
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
        });
    }).detach();
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

/* ================================================================== freeplay scene */

int ReplayLab::GameKind() const { return replay ? KindOfBall(replay->ballArch) : KIND_SOCCAR; }

// the stadium the game is in is the replay's (same map file: day, dusk, rain... are different ones), in its mode
bool ReplayLab::InReplayStadium() const {
    auto lower = [](std::string x) {
        std::transform(x.begin(), x.end(), x.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return x;
    };
    if (!replay || lower(gameWrapper->GetCurrentMap()) != lower(replay->map)) return false;
    ServerWrapper sw = gameWrapper->GetGameEventAsServer();
    BallWrapper ball = sw.IsNull() ? BallWrapper(0) : sw.GetBall();
    return ball.IsNull() || KindOfRadius(ball.GetRadius()) == GameKind();
}

void ReplayLab::EnterFreeplay(std::function<void()> then) {
    if (gameWrapper->IsInFreeplay() && !gameWrapper->GetLocalCar().IsNull() && InReplayStadium()) {
        then();
        return;
    }
    if (gameWrapper->IsInOnlineGame() && !gameWrapper->IsInReplay()) {
        Status("Pas pendant une partie en ligne.");
        return;
    }
    // the freeplay of the replay's stadium: the bots and cars of the old one go with it
    carsReady = false;
    botSlot.clear();
    dressed.clear();
    paintedAt.clear();
    lastValid.clear();
    dollyAsked = false;
    localStashed = false;
    cvarManager->log("Replay Lab: stade actuel " + gameWrapper->GetCurrentMap() + ", celui du replay " + replay->map + " (" +
                     KIND_LABELS[GameKind()] + ", " + replay->ballArch + ")");
    Status("Chargement du stade du replay (" + replay->map + ", " + KIND_LABELS[GameKind()] + ")...", 20.f);
    gameWrapper->ExecuteUnrealCommand("start " + replay->map + "?Game=" + KIND_CLASS[GameKind()] + "?GameTags=Freeplay");
    WaitForFreeplay(0, then);
}

void ReplayLab::WaitForFreeplay(int tries, std::function<void()> then) {
    gameWrapper->SetTimeout([this, tries, then](GameWrapper* gw) {
        // in the new stadium (not still the old one; a map named otherwise is accepted after 20 s)
        if (gw->IsInFreeplay() && !gw->GetLocalCar().IsNull() && (InReplayStadium() || tries > 40)) {
            gw->SetTimeout([then](GameWrapper*) { then(); }, 1.f);   // let the spawn settle
            return;
        }
        if (tries == 30 && replay)   // nothing after 15 s: asked again (a default freeplay would be the wrong stadium)
            gw->ExecuteUnrealCommand("start " + replay->map + "?Game=" + KIND_CLASS[GameKind()] + "?GameTags=Freeplay");
        if (tries == 30) Status("Lance l'entrainement libre toi-meme: Replay Lab continuera tout seul.", 60.f);
        if (tries < 240) WaitForFreeplay(tries + 1, then);
    }, 0.5f);
}

int ReplayLab::ToiIndex() const {
    for (size_t k = 0; k < roles.size(); k++)
        if (roles[k] == ROLE_TOI) return static_cast<int>(k);
    return -1;
}

// Your own car stands for the player you drive ("Toi"), and for nobody when you watch: then every player has his own
// car (his body, his look) and yours waits under the field while the camera follows theirs.
int ReplayLab::CarPlayer() const { return ToiIndex(); }

// the scene turned around (rlab_mirror): only for an orange player you drive
bool ReplayLab::Turned() const {
    return cvarManager->getCvar("rlab_mirror").getBoolValue() && CarPlayer() >= 0 && replay->teams[CarPlayer()] == 1;
}

int ReplayLab::BotIndex(CarWrapper car) const {
    auto it = botSlot.find(car.memory_address);
    return it == botSlot.end() ? -1 : it->second;
}

// Everyone but your own car's player has a bot; while simulating, removed players have none.
bool ReplayLab::NeedsBot(int k, bool forSim) const {
    return k != CarPlayer() && !(forSim && roles[k] == ROLE_REMOVED);
}

// Spawn the bots that are missing, take away the ones that must not be there, then run `then`.
void ReplayLab::SetupCars(std::function<void()> then, bool forSim) {
    std::lock_guard<std::recursive_mutex> lock(mtx);
    ServerWrapper sw = gameWrapper->GetGameEventAsServer();
    if (!replay || sw.IsNull() || !gameWrapper->IsInFreeplay()) return;
    // Bots are never taken out of the game (that leaves controllers behind that crash the next SpawnBot). A bot keeps the
    // body it was made with: each player gets a bot of his body, with his name (the others, LabR..., wait under the field),
    // and a bot that wore another look gets this one when it comes back from a demolition (the game paints a car only when
    // it builds it). Only when no bot has the body is one made, dressed while the game builds it (OnSpawnCar).
    ArrayWrapper<CarWrapper> cars = sw.GetCars();
    std::vector<CarWrapper> labs;
    for (int i = 0; i < cars.Count(); i++) {
        CarWrapper car = cars.Get(i);
        if (!car.GetbHidden() && OurBot(car)) labs.push_back(car);
    }
    auto taken = [&](const std::string& name) {
        return std::any_of(labs.begin(), labs.end(), [&](CarWrapper c) { return NameOf(c) == name; });
    };
    auto rename = [&](CarWrapper car, const std::string& name) {
        dressed.erase(NameOf(car));
        car.GetPRI().eventSetPlayerName(name);
    };
    std::vector<bool> served(replay->Players(), false);
    std::vector<bool> used(labs.size(), false);
    // first the bots that already have the right name and body
    for (size_t i = 0; i < labs.size(); i++) {
        for (int k = 0; k < replay->Players(); k++) {
            if (served[k] || NameOf(labs[i]) != BotName(k) || labs[i].GetLoadoutBody() != replay->looks[k].Body()) continue;
            served[k] = used[i] = true;
            break;
        }
    }
    // then any bot of ours with the right body, renamed; a bot in the way of a name is set aside
    int spareNo = 0;
    for (int k = 0; k < replay->Players(); k++) {
        if (served[k] || !NeedsBot(k, forSim)) continue;
        for (size_t i = 0; i < labs.size(); i++) {
            if (used[i] || labs[i].GetLoadoutBody() != replay->looks[k].Body()) continue;
            for (size_t j = 0; j < labs.size(); j++)
                if (!used[j] && j != i && NameOf(labs[j]) == BotName(k)) {
                    std::string spare;
                    do spare = "LabR" + std::to_string(++spareNo);
                    while (taken(spare));
                    rename(labs[j], spare);
                }
            rename(labs[i], BotName(k));
            served[k] = used[i] = true;
            break;
        }
    }
    // the bots kept that wore another look: this one, from their next car
    for (size_t i = 0; i < labs.size(); i++) {
        if (!used[i]) continue;
        for (int k = 0; k < replay->Players(); k++)
            if (NameOf(labs[i]) == BotName(k) && dressed[BotName(k)] != replay->id + "/" + std::to_string(k)) Redress(labs[i], k);
    }
    sw.SetbRandomizedBotLoadouts(0);   // our bots wear the players' looks, not random ones
    std::vector<int> missing;
    for (int k = 0; k < replay->Players(); k++)
        if (NeedsBot(k, forSim) && !served[k]) missing.push_back(k);
    if (!missing.empty()) Status("Ajout de " + std::to_string(missing.size()) + " voiture(s)...", 8.f);
    // cars blown up to take their look are away for 3 s: a simulation starting without them would take them for
    // demolished (and play them as bots)
    auto ready = [this, then, forSim] {
        const float wait = std::chrono::duration<float>(redressedUntil - std::chrono::steady_clock::now()).count();
        if (wait <= 0.f) {
            then();
            return;
        }
        gameWrapper->SetTimeout([this, then, forSim](GameWrapper*) {
            std::lock_guard<std::recursive_mutex> lock(mtx);
            if (!replay) return;
            MapBots(forSim);
            then();
        }, wait);
    };
    SpawnBots(missing, 0, ready, forSim);
}

// The look and team of player k on a bot's player info, then its car blown up: it comes back built with them (3 s).
void ReplayLab::Redress(CarWrapper car, int k) {
    PriWrapper pri = car.GetPRI();
    if (pri.IsNull()) return;
    Dress(pri, k);
    car.Demolish();
    redressedUntil = std::chrono::steady_clock::now() + std::chrono::milliseconds(3400);
    Status("Les voitures prennent l'apparence des joueurs (3 s)...", 4.f);
}

// a bot's player info takes the look of player k (as the game builds its next car from it) and his team
void ReplayLab::Dress(PriWrapper pri, int k) {
    ServerWrapper sw = gameWrapper->GetGameEventAsServer();
    if (sw.IsNull() || !replay) return;
    const Look& look = replay->looks[k];
    if (look.known) {
        BotLoadoutData data;
        for (const auto& [slot, product, paint] : look.slots) {
            data.products[slot] = product;
            if (paint >= 0) data.product_attributes[slot].push_back({BotLoadoutData::Attribute::Type::PAINT, paint});
        }
        if (look.teamFinish > 0) data.products[7] = look.teamFinish;      // paint finish
        if (look.accentFinish > 0) data.products[12] = look.accentFinish; // accent finish
        data.team = static_cast<unsigned char>(GameTeam(k));
        data.team_color_id = static_cast<unsigned char>(look.teamColor);
        data.custom_color_id = static_cast<unsigned char>(look.accentColor);
        data.team_finish_id = look.teamFinish;
        data.custom_finish_id = look.accentFinish;
        gameWrapper->SetBotLoadout(pri, data);
    }
    // his team too: a car moved to another team afterwards takes the default colours
    ArrayWrapper<TeamWrapper> teams = sw.GetTeams();
    if (GameTeam(k) < teams.Count() && !teams.Get(GameTeam(k)).IsNull()) pri.SetPlayerTeam(teams.Get(GameTeam(k)));
    dressed[BotName(k)] = replay->id + "/" + std::to_string(k);
    cvarManager->log("Replay Lab: bot a l'image de " + replay->names[k] + " (couleurs " + std::to_string(look.teamColor) + "/" +
                     std::to_string(look.accentColor) + ", " + std::to_string(look.slots.size()) + " objets, equipe " + std::to_string(GameTeam(k)) +
                     ")");
}

// One bot at a time, each waited for: several SpawnBot in the same frame lose every other bot.
void ReplayLab::SpawnBots(std::vector<int> missing, int tries, std::function<void()> then, bool forSim) {
    std::lock_guard<std::recursive_mutex> lock(mtx);
    ServerWrapper sw = gameWrapper->GetGameEventAsServer();
    if (sw.IsNull() || !replay) return;
    auto there = [&](const std::string& name) {   // a bot's car (yours may have the same name: you played in the replay)
        ArrayWrapper<CarWrapper> cars = sw.GetCars();
        for (int i = 0; i < cars.Count(); i++)
            if (!cars.Get(i).GetbHidden() && OurBot(cars.Get(i)) && NameOf(cars.Get(i)) == name) return true;
        return false;
    };
    auto exists = [&](const std::string& name) {   // its player info, car or not (coming back from a demolition)
        ArrayWrapper<PriWrapper> pris = sw.GetPRIs();
        for (int i = 0; i < pris.Count(); i++)
            if (!pris.Get(i).IsNull() && pris.Get(i).GetbBot() && pris.Get(i).GetPlayerName().ToString() == name) return true;
        return false;
    };
    while (!missing.empty() && there(BotName(missing.front()))) {
        missing.erase(missing.begin());
        tries = 0;
    }
    if (missing.empty()) {
        MapBots(forSim);
        then();
        return;
    }
    const int k = missing.front();
    if (tries == 40) {   // 4 s without it: on to the next
        Status(replay->names[k] + " n'a pas pu etre ajoute.", 10.f);
        missing.erase(missing.begin());
        tries = 0;
    } else if (tries % 10 == 0 && !exists(BotName(k))) {   // asked, then again every second until it is there
        spawning = k;   // OnSpawnCar dresses it
        sw.SpawnBot(replay->looks[k].Body(), BotName(k));
        spawning = -1;
    }
    gameWrapper->SetTimeout([this, missing, tries, then, forSim](GameWrapper*) { SpawnBots(missing, tries + 1, then, forSim); }, 0.1f);
}

// Give each needed player one of our bots; every other one (a removed player, the player your
// own car stands for, a duplicate from a late spawn, a leftover from another replay) is set aside.
// Bots are never taken out of the game: RemovePlayer crashes BakkesMod in freeplay.
void ReplayLab::MapBots(bool forSim) {
    botSlot.clear();
    ServerWrapper sw = gameWrapper->GetGameEventAsServer();
    if (sw.IsNull() || !replay) return;
    std::vector<bool> taken(replay->Players(), false);
    ArrayWrapper<CarWrapper> cars = sw.GetCars();
    for (int i = 0; i < cars.Count(); i++) {
        CarWrapper car = cars.Get(i);
        if (car.IsNull()) continue;
        if (!OurBot(car)) continue;
        const std::string name = NameOf(car);
        if (car.GetbHidden() || car.GetbDeleteMe()) {   // demolished: its player comes back in a new car
            botSlot[car.memory_address] = DEAD;
            continue;
        }
        int slot = PARKED;
        for (int k = 0; k < replay->Players(); k++)
            if (name == BotName(k) && NeedsBot(k, forSim) && !taken[k]) { slot = k; taken[k] = true; }
        botSlot[car.memory_address] = slot;
    }
    carsReady = true;
    DressCars();
}

// Plain characters only (the game may change a name with others, and the bot would no longer be found by it); a
// second player of the same name gets a number: each bot has its own name.
std::string ReplayLab::BotName(int k) const {
    auto plain = [](const std::string& name) {
        std::string out;
        for (unsigned char c : name)
            if (c >= 32 && c < 127 && out.size() < 24) out += static_cast<char>(c);
        return out;
    };
    std::string name = plain(replay->names[k]);
    if (name.empty() || name == "LabCam" || name.compare(0, 4, "LabR") == 0) name = "Joueur " + std::to_string(k + 1);
    int same = 0;
    for (int j = 0; j < k; j++)
        if (plain(replay->names[j]) == plain(replay->names[k])) same++;
    return same ? name + " (" + std::to_string(same + 1) + ")" : name;
}

bool ReplayLab::OurBot(CarWrapper car) const {
    return !car.IsNull() && !car.GetPRI().IsNull() && car.GetPRI().GetbBot() && NameOf(car) != "LabCam";
}

int ReplayLab::GameTeam(int k) const {
    return replay->teams[k] ^ (Turned() ? 1 : 0);
}

// Each bot wears the look of the player it plays (body, decal, wheels, boost, antenna, topper, paints, team colours)
// and joins his team; your car joins the team of the player it stands for (your own look stays yours).
void ReplayLab::DressCars() {
    ServerWrapper sw = gameWrapper->GetGameEventAsServer();
    if (sw.IsNull() || !replay) return;
    ArrayWrapper<TeamWrapper> teams = sw.GetTeams();
    // a player joins a team: its info first, then the car takes the team's colours
    auto join = [&](CarWrapper car, PriWrapper pri, int team) {
        if (pri.IsNull() || team >= teams.Count() || teams.Get(team).IsNull()) return;
        if (!car.IsNull() && car.GetTeamNum2() == team) return;
        const std::string who = pri.GetPlayerName().ToString();
        pri.SetPlayerTeam(teams.Get(team));
        if (!car.IsNull()) {
            car.HandleTeamChanged(pri);
            car.InitTeamPaint();
        }
        cvarManager->log("Replay Lab: " + who + " passe dans l'equipe " + std::to_string(team));
    };
    PlayerControllerWrapper pc = gameWrapper->GetPlayerController();
    if (!pc.IsNull() && CarPlayer() >= 0) join(gameWrapper->GetLocalCar(), pc.GetPRI(), GameTeam(CarPlayer()));
    ArrayWrapper<CarWrapper> cars = sw.GetCars();
    for (int i = 0; i < cars.Count(); i++) {
        CarWrapper car = cars.Get(i);
        const int k = car.IsNull() || car.GetbHidden() ? -1 : BotIndex(car);
        if (k >= 0) join(car, car.GetPRI(), GameTeam(k));
    }
}

// Inside SpawnBot the game makes the bot's player info, then (GameEvent_TA.SpawnCar) its car, and paints the car from
// the bot's look (HandleLoadoutSelected, InitTeamPaint, SetLoadout). Just before the car: the bot we are making gets the
// look of its player and his team (a car moved to another team afterwards takes the default colours). It has no name of
// ours yet: it is the bot without a car.
void ReplayLab::OnSpawnCar() {
    std::lock_guard<std::recursive_mutex> lock(mtx);
    if (spawning < 0 || !replay) return;
    ServerWrapper sw = gameWrapper->GetGameEventAsServer();
    if (sw.IsNull()) return;
    ArrayWrapper<PriWrapper> pris = sw.GetPRIs();
    PriWrapper pri(0);
    for (int i = 0; i < pris.Count(); i++)
        if (!pris.Get(i).IsNull() && pris.Get(i).GetbBot() && pris.Get(i).GetCar().IsNull()) pri = pris.Get(i);   // the newest
    if (pri.IsNull()) return;
    Dress(pri, spawning);
    spawning = -1;   // one car per bot
}


// From a simulation (where removed players have no bot), the replay needs everybody back first.
void ReplayLab::PlayFrom(float t, bool play) {
    SetupCars([this, t, play] { StartPlayback(t, play); }, false);
}

void ReplayLab::StartPlayback(float t, bool play) {
    std::lock_guard<std::recursive_mutex> lock(mtx);
    if (!replay) return;
    mirrored = Turned();
    mode = MODE_PLAYBACK;
    rt = std::min(std::max(t, Start()), End());
    playing = play;
    ballLocked = false;
    waiting = false;
    lastWorld = -1.f;
    SetGoals(false);   // goals are ours: freeplay's goal explosion would demolish the cars and reset the ball
    BallBack(gameWrapper->GetGameEventAsServer());
    cvarManager->executeCommand("sv_soccar_gamespeed 1", false);
}

void ReplayLab::Fork() {
    std::lock_guard<std::recursive_mutex> lock(mtx);
    if (!replay || mode == MODE_IDLE || !carsReady) {
        Status("Ouvre d'abord un replay (F3).");
        return;
    }
    ServerWrapper sw = gameWrapper->GetGameEventAsServer();
    CarWrapper local = gameWrapper->GetLocalCar();
    if (sw.IsNull()) return;
    const int toi = ToiIndex();
    mirrored = Turned();
    mode = MODE_SIM;
    forkT = rt;
    simT = 0.f;
    playing = false;
    lastWorld = -1.f;
    ballLocked = cvarManager->getCvar("rlab_sync").getBoolValue();
    releaseT = ballLocked ? -1.f : 0.f;
    goalTeam = -1;
    clockAtFork = RemainingAt(forkT);
    inOvertime = OvertimeAt(forkT);
    overtimeT = inOvertime ? -clockAtFork : 0.f;
    clockStop = -1.f;
    match = inOvertime ? MATCH_OVERTIME : clockAtFork <= 0.f ? MATCH_ZERO : MATCH_PLAYING;
    matchResult.clear();
    BallBack(sw);
    demoed.assign(replay->names.size(), false);
    nextoAction.assign(replay->names.size(), nexto::Action{});
    boostingNow.assign(replay->names.size(), 0);
    scenario = nullptr;   // a new recording starts with this simulation
    nextoTick = nexto::TICK_SKIP - 1;
    // like custom training: when you drive, everything waits at that moment until you move
    waiting = toi >= 0 && cvarManager->getCvar("rlab_attendre").getBoolValue();

    MapBots(true);   // removed players' bots are set aside
    PlaceAtFork(!waiting);
    for (int k = 0; k < replay->Players(); k++) {   // who is in the air without his flip, for the log
        const Air a = AirAt(k, forkT);
        if (a.jumped || a.flipped)
            cvarManager->log("Replay Lab: au depart " + replay->names[k] + (a.flipped ? " a deja utilise son flip"
                             : a.sinceJump < DODGE_WINDOW ? " a saute (flip encore " + std::to_string(DODGE_WINDOW - a.sinceJump).substr(0, 4) + " s)"
                                                          : " a saute il y a plus de 1,25 s: plus de flip"));
    }
    if (match == MATCH_ZERO) Status("0:00 : le match se termine des que le ballon touche le sol, un but avant compte encore.", 8.f);
    cvarManager->executeCommand("sv_soccar_gamespeed " + std::to_string(cvarManager->getCvar("rlab_speed").getFloatValue()), false);
    if (waiting) Status("Simulation prete: elle demarre des que tu bouges ta manette, comme l'entrainement. F11 recommencer, F9 retour au replay.", 8.f);
    else Status(toi >= 0 ? "Simulation: tu conduis " + replay->names[toi] + ". F11 recommencer, F4 autre voiture, F9 retour au replay."
                         : "Simulation: tu regardes (" + CamText() + "). F11 recommencer, F9 retour au replay, F4 joueur suivant.");
}

// Every car and the ball where they are at the moment the simulation starts from; still (waiting for you) or moving.
void ReplayLab::PlaceAtFork(bool moving) {
    ServerWrapper sw = gameWrapper->GetGameEventAsServer();
    if (sw.IsNull() || !replay) return;
    auto at = [&](int k) {
        State s = StateAt(k, forkT);
        if (!moving) s.vel = s.ang = Vector(0, 0, 0);
        return s;
    };
    CarWrapper local = gameWrapper->GetLocalCar();
    if (!local.IsNull() && CarPlayer() >= 0) {
        const State me = at(CarPlayer());
        if (me.valid) {
            ApplyCar(local, me);
            ApplyAir(local, AirAt(CarPlayer(), forkT));
        }
    } else if (!local.IsNull()) {
        Stash(local, true);
    }
    ArrayWrapper<CarWrapper> cars = sw.GetCars();
    for (int i = 0; i < cars.Count(); i++) {
        CarWrapper car = cars.Get(i);
        if (car.IsNull() || (!local.IsNull() && Same(car, local))) continue;
        const int k = BotIndex(car);
        if (k == PARKED) Park(car);
        if (k < 0) continue;
        const State s = at(k);
        if (s.valid) {
            ApplyCar(car, s);
            ApplyAir(car, AirAt(k, forkT));
        } else {
            Park(car);
        }
    }
    BallWrapper ball = sw.GetBall();
    const State b = at(-1);
    if (!ball.IsNull() && b.valid) {
        ball.SetLocation(b.pos);
        ball.SetVelocity(b.vel);
        ball.SetAngularVelocity(b.ang, false);
    }
}

void ReplayLab::BackToReplay() {
    PlayFrom(forkT, false);
    Status("Retour au replay (F9 pour lancer la lecture).");
}

void ReplayLab::TogglePlay() {
    std::lock_guard<std::recursive_mutex> lock(mtx);
    if (mode == MODE_SIM) {
        BackToReplay();
        return;
    }
    if (mode != MODE_PLAYBACK) {
        Status(gameWrapper->IsInReplay() ? "F5 (ou F10) pour simuler depuis ce moment du replay." : "Ouvre d'abord un replay (F3).");
        return;
    }
    if (!playing && rt >= End() - 0.05f) rt = Start();
    playing = !playing;
    lastWorld = -1.f;
}

void ReplayLab::Seek(float t) {
    std::lock_guard<std::recursive_mutex> lock(mtx);
    if (!replay) return;
    if (mode == MODE_SIM) PlayFrom(t, false);
    else rt = std::min(std::max(t, Start()), End());
}

void ReplayLab::OnSimulateKey() {
    if (gameWrapper->IsInReplay()) {
        OpenFromViewer();
        return;
    }
    std::lock_guard<std::recursive_mutex> lock(mtx);
    if (mode == MODE_PLAYBACK) Fork();
    else if (mode == MODE_SIM) SetupCars([this] { rt = forkT; Fork(); }, true);
    else Status("Ouvre d'abord un replay (F3), ou appuie sur F5 dans le visionneur de replays.");
}

// F4: while you drive a simulation, you take the next player's car, from the same moment. Otherwise the camera
// follows the next player, in his own car.
void ReplayLab::NextView() {
    std::lock_guard<std::recursive_mutex> lock(mtx);
    if (!replay || mode == MODE_IDLE) return;
    const int n = replay->Players();
    const int toi = ToiIndex();
    if (mode == MODE_SIM && toi >= 0) {
        roles[toi] = ROLE_GHOST;
        roles[(toi + 1) % n] = ROLE_TOI;
        SetupCars([this] {
            rt = forkT;
            Fork();
        }, true);
        return;
    }
    if (camMode != CAM_PLAYER && camMode != CAM_AUTO) camMode = CAM_PLAYER;
    if (camMode == CAM_AUTO) camMode = CAM_PLAYER;   // you pick: the director stops
    NextCamTarget();
}

/* ================================================================== every frame / every car input */

void ReplayLab::ApplyCar(CarWrapper car, const State& s) {
    car.SetLocation(s.pos);
    car.SetRotation(s.rot);
    car.SetVelocity(s.vel);
    car.SetAngularVelocity(s.ang, false);
    BoostWrapper boost = car.GetBoostComponent();
    if (!boost.IsNull()) boost.SetBoostAmount(s.boost);
}

// What player k still has in the air at moment t, from the jumps, double jumps and dodges the replay marks since he
// last touched the floor, a wall or the ceiling. A flip comes back when the ball touches his wheels (a flip reset):
// seen in the replay, or deduced from a second flip before landing (the reset was where he passed closest to the ball).
Air ReplayLab::AirAt(int k, float t) const {
    Air a;
    if (!replay || k < 0 || k >= replay->Players() || replay->moves.empty()) return a;
    const ReplayData& R = *replay;
    const size_t P = static_cast<size_t>(R.Players());
    const int f = FrameAt(t), n = R.Frames(), kind = GameKind();
    auto car = [&](int g) -> const RBody& { return R.cars[g * P + k]; };
    auto moves = [&](int g) { return R.moves[g * P + k]; };
    if (!car(f).valid || OnSurface(car(f), kind)) return a;   // on a surface the game gives everything back itself
    int flipStart = -1;   // the last flip before t
    bool inJump = false, flipSeen = false;
    for (int g = f; g >= 0 && car(g).valid; g--) {
        const unsigned char m = moves(g);
        if (m & 1) {
            if (!a.jumped) a.sinceJump = std::max(0.f, t - R.time[g]);
            a.jumped = inJump = true;
            continue;
        }
        if (inJump) break;   // just before the jump: on a surface
        if (m & 6) {
            if (!flipSeen) flipStart = g;
            a.flipped = true;
        } else if (flipStart >= 0) {
            flipSeen = true;
        }
        if (OnSurface(car(g), kind)) break;
    }
    if (!a.flipped) return a;
    for (int g = flipStart + 1; g <= f; g++)
        if (!(moves(g) & 6) && WheelsOnBall(car(g), R.ball[g])) return Air{};
    int h = f + 1;
    while (h < n && car(h).valid && (moves(h) & 6)) h++;   // the rest of the flip going on
    for (; h < n && car(h).valid && !(moves(h) & 1) && !OnSurface(car(h), kind); h++) {
        if (!(moves(h) & 6)) continue;
        int closest = -1;
        float best = 300.f;
        for (int g = flipStart + 1; g < h; g++) {
            if ((moves(g) & 6) || !R.ball[g].valid) continue;
            const float d = Len(Sub(R.ball[g].pos, car(g).pos));
            if (d < best) {
                best = d;
                closest = g;
            }
        }
        if (closest >= 0 && closest <= f) return Air{};
        break;
    }
    return a;
}

// A car moved to the moment a simulation starts from would keep the jump and flip it had (from the last simulation), or
// get them all back in the air: it gets the replay's. The flip window after a jump closes when it did in the replay.
void ReplayLab::ApplyAir(CarWrapper car, const Air& a) {
    if (car.IsNull()) return;
    const bool window = a.jumped && !a.flipped && a.sinceJump < DODGE_WINDOW;
    car.SetbJumped(a.jumped || a.flipped ? 1 : 0);
    car.SetbDoubleJumped(a.flipped || (a.jumped && !window) ? 1 : 0);
    JumpComponentWrapper jump = car.GetJumpComponent();
    if (window && !jump.IsNull()) jump.SetActivityTime(a.sinceJump);   // time in its state: since the jump ended
}

// While the replay plays: no demolitions (cars following it overlap where real players bumped), boost set by the replay.
// In a simulation it is a real match: demolitions, and a car that drives itself has limited boost (freeplay's is unlimited).
void ReplayLab::CarRules(CarWrapper car, bool drivesItself) {
    const int target = mode == MODE_SIM ? normalDemo : noDemo;
    if (target >= 0 && car.GetDemolishTarget() != target) car.SetDemolishTarget(static_cast<unsigned char>(target));
    BoostWrapper boost = car.GetBoostComponent();
    if (boost.IsNull()) return;
    const int unlimited = boost.GetUnlimitedBoostRefCount();
    if (mode == MODE_SIM && drivesItself) {
        if (unlimited > 0) {
            freeplayBoost = unlimited;
            boost.SetUnlimitedBoostRefCount(0);
        }
    } else if (unlimited == 0 && freeplayBoost > 0) {
        boost.SetUnlimitedBoostRefCount(freeplayBoost);
    }
}

int ReplayLab::RoleOf(int k) const {
    // a player demolished in the simulation no longer follows the replay: back in a new car, the Psyonix AI plays him
    if (mode == MODE_SIM && k < static_cast<int>(demoed.size()) && demoed[k] && roles[k] == ROLE_GHOST) return ROLE_BOT;
    if (roles[k] == ROLE_NEXTO && !nextoModel.Loaded()) return ROLE_BOT;
    return roles[k];
}

bool ReplayLab::DrivesItself(int k) const {
    const int r = RoleOf(k);
    return r == ROLE_BOT || r == ROLE_FREE || r == ROLE_NEXTO;
}

/* ================================================================== Nexto */

static nexto::Vec NV(const Vector& v) { return nexto::Vec{v.X, v.Y, v.Z}; }

// Every 8 physics ticks each Nexto car picks its next action from what it sees of the game: the cars on the pitch
// (teams as in the replay, swapped if the scene is turned around), the ball and the boost pads that are up.
void ReplayLab::NextoDecide(ServerWrapper sw, CarWrapper local) {
    BallWrapper b = sw.GetBall();
    if (b.IsNull()) return;
    const nexto::Ball ball{NV(b.GetLocation()), NV(b.GetVelocity()), NV(b.GetAngularVelocity())};
    std::vector<nexto::Car> cars;
    std::vector<int> player;
    auto add = [&](CarWrapper car, int k) {
        nexto::Car c;
        c.team = replay->teams[k] ^ (mirrored ? 1 : 0);
        c.pos = NV(car.GetLocation());
        c.vel = NV(car.GetVelocity());
        c.ang = NV(car.GetAngularVelocity());
        const Axes ax = AxesOf(car.GetRotation());
        c.fwd = NV(ax.f);
        c.up = NV(ax.u);
        BoostWrapper boost = car.GetBoostComponent();
        c.boost = boost.IsNull() ? 0.0 : boost.GetCurrentBoostAmount();
        c.onGround = car.IsOnGround();
        c.hasFlip = car.HasFlip() != 0;
        cars.push_back(c);
        player.push_back(k);
    };
    if (!local.IsNull() && CarPlayer() >= 0) add(local, CarPlayer());
    ArrayWrapper<CarWrapper> list = sw.GetCars();
    for (int i = 0; i < list.Count(); i++) {
        CarWrapper car = list.Get(i);
        if (car.IsNull() || (!local.IsNull() && Same(car, local)) || car.GetbHidden()) continue;
        const int k = BotIndex(car);
        if (k >= 0) add(car, k);
    }
    float up[nexto::PADS];
    PadStates(sw, up);
    for (size_t i = 0; i < cars.size(); i++) {
        const int k = player[i];
        if (RoleOf(k) != ROLE_NEXTO) continue;
        nextoAction[k] = nexto::Model::Table()[nextoModel.Act(cars, static_cast<int>(i), ball, up, nextoAction[k])];
    }
}

void ReplayLab::NextoInput(int k, ControllerInput* in) const {
    const nexto::Action& a = nextoAction[k];
    in->Throttle = a[0];
    in->Steer = a[1];
    in->Pitch = a[2];
    in->Yaw = a[3];
    in->Roll = a[4];
    in->DodgeForward = -a[2];                                   // a flip goes where the stick points
    in->DodgeStrafe = std::min(std::max(a[3] + a[4], -1.f), 1.f);   // Nexto sideflips with roll
    in->Jump = a[5] > 0 ? 1 : 0;
    in->ActivateBoost = in->HoldingBoost = a[6] > 0 ? 1 : 0;
    in->Handbrake = a[7] > 0 ? 1 : 0;
}

// A pad being picked up tells us where it is: from then on its state is read straight from the game.
void ReplayLab::OnPadPickedUp(ActorWrapper pad) {
    std::lock_guard<std::recursive_mutex> lock(mtx);
    ServerWrapper sw = gameWrapper->GetGameEventAsServer();
    if (pad.IsNull() || sw.IsNull()) return;
    if (sw.memory_address != padServer) {
        padServer = sw.memory_address;
        pads.clear();
    }
    if (pads.count(pad.memory_address)) return;
    const Vector p = pad.GetLocation();
    int best = -1;
    double bestD = 300.0 * 300.0;
    for (int i = 0; i < nexto::PADS; i++) {
        const double dx = p.X - nexto::PAD_LOCATIONS[i][0], dy = p.Y - nexto::PAD_LOCATIONS[i][1], d = dx * dx + dy * dy;
        if (d < bestD) {
            bestD = d;
            best = i;
        }
    }
    if (best >= 0) pads[pad.memory_address] = best;
}

void ReplayLab::PadStates(ServerWrapper sw, float up[nexto::PADS]) {
    std::fill(up, up + nexto::PADS, 1.f);
    if (sw.memory_address != padServer) {   // another game: those pads are gone
        padServer = sw.memory_address;
        pads.clear();
    }
    for (const auto& [address, index] : pads)
        if (VehiclePickupWrapper(address).GetbPickedUp()) up[index] = 0.f;
}

// "What if he had been demolished here": blow up that player's car now, in the simulation going on.
void ReplayLab::DemolishPlayer(int k) {
    std::lock_guard<std::recursive_mutex> lock(mtx);
    if (!replay || mode != MODE_SIM || k < 0 || k >= replay->Players()) {
        Status("Demolir: seulement pendant une simulation (F5).");
        return;
    }
    ServerWrapper sw = gameWrapper->GetGameEventAsServer();
    CarWrapper local = gameWrapper->GetLocalCar();
    CarWrapper target = k == CarPlayer() ? local : CarWrapper(0);
    if (k != CarPlayer() && !sw.IsNull()) {
        ArrayWrapper<CarWrapper> cars = sw.GetCars();
        for (int i = 0; i < cars.Count(); i++)
            if (!cars.Get(i).IsNull() && BotIndex(cars.Get(i)) == k) target = cars.Get(i);
    }
    if (target.IsNull()) {
        Status(replay->names[k] + " n'est pas sur le terrain.");
        return;
    }
    if (roles[k] != ROLE_TOI) {
        demoed[k] = true;
        Status(replay->names[k] + " est demoli !" + (roles[k] == ROLE_GHOST && k != CarPlayer() ? " Il reapparait et joue en bot Psyonix." : ""), 10.f);
    }
    target.Demolish();
}

void ReplayLab::OnDemolished(CarWrapper car) {
    if (car.IsNull()) return;
    std::lock_guard<std::recursive_mutex> lock(mtx);
    if (!replay || mode != MODE_SIM) return;
    CarWrapper local = gameWrapper->GetLocalCar();
    MarkDemolished(!local.IsNull() && Same(car, local) ? CarPlayer() : BotIndex(car));
}

void ReplayLab::MarkDemolished(int k) {
    if (k < 0 || k >= static_cast<int>(demoed.size()) || demoed[k]) return;
    for (float dt = 0.f; dt < 0.6f; dt += 0.05f)
        if (!StateAt(k, Now() + dt).valid) return;   // demolished in the replay too: back in a new car, he follows it again
    demoed[k] = true;
    if (roles[k] == ROLE_TOI) Status("Demoli ! Tu reapparais dans 3 secondes.");
    else if (roles[k] == ROLE_GHOST && k != CarPlayer())
        Status(replay->names[k] + " est demoli ! Il reapparait et joue en bot Psyonix.", 10.f);
    else Status(replay->names[k] + " est demoli !", 10.f);
}

// In a simulation a ghost is driven along its real path by its speed rather than put on it: it collides like a car
// (a bump moves it, it pushes back) and comes back to the path; knocked too far from it, it is put back.
void ReplayLab::TrackCar(CarWrapper car, const State& s) {
    const Vector off = Sub(s.pos, car.GetLocation());
    if (Len(off) > 200.f) {
        ApplyCar(car, s);
        return;
    }
    car.SetVelocity(Add(s.vel, Mul(off, 12.f)));   // its real speed, plus the way back (most of it within 0.1 s)
    car.SetRotation(s.rot);
    car.SetAngularVelocity(s.ang, false);
    BoostWrapper boost = car.GetBoostComponent();
    if (!boost.IsNull()) boost.SetBoostAmount(s.boost);
}

// The game paints our bots white (their colours do not come through SetBotLoadout): each gets its player's team colour
// and accent colour from the game's palettes.
void ReplayLab::PaintCar(CarWrapper car, int k) {
    const Look& look = replay->looks[k];
    if (!look.known || look.teamColor >= 70 || look.accentColor >= 105) return;
    if (GameTeam(k) == 1) {
        // SetCarColor does nothing on orange cars: the game paints them itself from their player info, once the car is there
        PriWrapper pri = car.GetPRI();
        if (pri.IsNull()) return;
        Dress(pri, k);
        car.InitTeamPaint();
        return;
    }
    const unsigned char* main = palette::BLUE[look.teamColor];
    const float gamma = cvarManager->getCvar("rlab_gamma").getFloatValue();
    car.SetCarColor(Paint(main, gamma), Paint(palette::ACCENT[look.accentColor], gamma));
}

// stays where it is (a player demolished at that moment of the replay)
void ReplayLab::Freeze(CarWrapper car) {
    car.SetVelocity(Vector(0, 0, 0));
    car.SetAngularVelocity(Vector(0, 0, 0), false);
}

// While you watch, your car is hidden where it is and goes through everything (under the field the game would take it
// away); when you drive it again it is back.
void ReplayLab::Stash(CarWrapper car, bool away) {
    if (away) {
        if (!car.GetbHidden()) car.SetHidden2(1);
        if (car.GetbCollideActors()) car.SetbCollideActors(0);
        if (car.GetbBlockActors()) car.SetbBlockActors(0);
        car.SetLocation(Vector(0.f, 0.f, 1900.f));   // high in the arena, out of the way
        Freeze(car);
        localStashed = true;
    } else if (localStashed) {
        car.SetHidden2(0);
        car.SetbCollideActors(1);
        car.SetbBlockActors(1);
        localStashed = false;
    }
}

// a bot nobody plays right now waits under the field, out of the way of the ball and the other cars
void ReplayLab::Park(CarWrapper car) {
    car.SetLocation(Vector(0.f, 0.f, PARK_Z));
    car.SetRotation(Rotator{0, 0, 0});
    Freeze(car);
}

void ReplayLab::SetGoals(bool enabled) {
    if (goalsOn == enabled) return;
    goalsOn = enabled;
    cvarManager->executeCommand("sv_freeplay_enablegoal_bakkesmod 1", false);
    cvarManager->executeCommand(std::string("sv_freeplay_enablegoal ") + (enabled ? "1" : "0"), false);
}

// Inputs: everything that follows the replay is driven by us, bots and your car in a simulation drive themselves.
// Your car's input is read once per physics tick (120 per second of game time, none while the game is paused; the
// bots' inputs come once per rendered frame): that is when everything that follows the replay moves, like the game
// moves its own cars.
void ReplayLab::OnCarInput(CarWrapper car, void* params) {
    if (car.IsNull() || params == nullptr) return;
    std::lock_guard<std::recursive_mutex> lock(mtx);
    if (!replay || !carsReady || mode == MODE_IDLE || !gameWrapper->IsInFreeplay()) return;
    auto* in = static_cast<ControllerInput*>(params);
    CarWrapper local = gameWrapper->GetLocalCar();
    // boosting or not, for the scenario being recorded
    auto note = [&](int who) {
        if (who >= 0 && who < static_cast<int>(boostingNow.size())) boostingNow[who] = in->ActivateBoost || in->HoldingBoost;
    };
    int k;
    if (!local.IsNull() && Same(car, local)) {
        tickSeen = true;
        Advance(TICK);
        Apply();
        if (mode == MODE_SIM && match == MATCH_OVER) {   // after the final whistle nobody drives
            ZeroInput(in);
            return;
        }
        if (mode == MODE_SIM && ToiIndex() >= 0) {   // you drive
            if (waiting) {
                if (!Moves(*in)) {   // like custom training: the scene waits for you
                    ZeroInput(in);
                    return;
                }
                waiting = false;
                PlaceAtFork(true);   // off: everything from that moment, at its own speed
                Status("C'est parti ! F11 recommencer, F9 retour au replay.", 3.f);
            }
            note(ToiIndex());
            return;
        }
        camInput = *in;   // your controller drives no car: it can fly the free camera
        k = CarPlayer();
        if (k < 0) {   // you watch: your car waits under the field
            ZeroInput(in);
            return;
        }
    } else {
        k = BotIndex(car);
        if (k == PARKED || (k == -1 && NameOf(car) == "LabCam")) ZeroInput(in);   // set aside, or the free camera's car
        if (k >= 0 && mode == MODE_SIM && match == MATCH_OVER) {
            ZeroInput(in);
            return;
        }
        if (k >= 0 && mode == MODE_SIM && waiting) {   // bots and Nexto wait for you too
            ZeroInput(in);
            return;
        }
        if (k >= 0 && mode == MODE_SIM && RoleOf(k) == ROLE_NEXTO) {
            NextoInput(k, in);
            note(k);
            return;
        }
        if (k < 0 || (mode == MODE_SIM && RoleOf(k) == ROLE_BOT)) {   // Psyonix AI drives
            note(k);
            return;
        }
    }
    const bool follows = mode == MODE_PLAYBACK || RoleOf(k) == ROLE_GHOST;
    ZeroInput(in);
    in->ActivateBoost = in->HoldingBoost = (follows && StateAt(k, Now()).boosting) ? 1 : 0;
    note(k);
}

// Every rendered frame: follows the game clock, and moves things itself only while no physics tick
// comes through your car (demolished, not spawned yet).
void ReplayLab::OnFrame() {
    std::lock_guard<std::recursive_mutex> lock(mtx);
    if (!replay || !carsReady || mode == MODE_IDLE || !gameWrapper->IsInFreeplay()) return;
    ServerWrapper sw = gameWrapper->GetGameEventAsServer();
    if (sw.IsNull()) return;
    // the game's own clock: it stops while the game is paused (Start menu) and slows down with the game speed
    WorldInfoWrapper world = sw.GetWorldInfo();
    const float wt = world.IsNull() ? lastWorld : world.GetTimeSeconds();
    float dt = lastWorld < 0.f || wt < lastWorld ? 0.f : std::min(wt - lastWorld, 0.1f);
    lastWorld = wt;
    if (gameWrapper->IsPaused()) dt = 0.f;
    if (tickSeen) {
        noTickTime = 0.f;
    } else if (dt > 0.f && (noTickTime += dt) > 0.1f) {
        Advance(dt);
        Apply();
    }
    tickSeen = false;
    UpdateCamera();
    AimCamera();   // also outside the car camera (after a demolition the game's camera watches the wreck)
}

void ReplayLab::Advance(float dt) {
    if (mode == MODE_SIM) {
        if (!waiting) simT += dt;   // game time: it already runs at the simulation speed (rlab_speed)
    } else if (playing) {
        rt += dt * playSpeed;
        if (rt >= End()) {
            rt = End();
            playing = false;
        }
    }
}

// Moves everything that follows the replay to where it is at this moment.
void ReplayLab::Apply() {
    ServerWrapper sw = gameWrapper->GetGameEventAsServer();
    if (sw.IsNull()) return;
    if (mode == MODE_SIM && waiting) {   // nothing moves before you do
        PlaceAtFork(false);
        return;
    }
    const float t = Now();
    const int toi = ToiIndex();
    applyTicks++;
    const bool flowing = t > lastApplyT && t - lastApplyT < 0.2f;   // the replay went on, no jump on the timeline
    lastApplyT = t;
    if (lastValid.size() != replay->names.size()) lastValid.assign(replay->names.size(), 0);
    // where things are in the replay; while the replay is paused they keep still instead of moving one tick each time
    const bool still = mode == MODE_PLAYBACK && !playing;
    auto at = [&](int k) {
        State s = StateAt(k, t);
        if (still) s.vel = s.ang = Vector(0, 0, 0);
        return s;
    };

    // rlab_debug 1: twice a second, where each car should be (replay) vs where the game has it
    const bool debug = cvarManager->getCvar("rlab_debug").getBoolValue() && (++debugFrame % 70 == 0);
    auto trace = [&](const std::string& who, CarWrapper car, const State& s) {
        if (!debug) return;
        const Vector a = car.GetLocation(), v = car.GetVelocity(), w = car.GetAngularVelocity();
        const Rotator r = car.GetRotation();
        BoostWrapper boost = car.GetBoostComponent();
        if (!boost.IsNull()) {
            char line[160];
            std::snprintf(line, sizeof line, "dbg %s boost=%.1f%% illimite=%d demolition=%d equipe=%d carrosserie=%d", who.c_str(),
                          100.f * boost.GetCurrentBoostAmount(), boost.GetUnlimitedBoostRefCount(), car.GetDemolishTarget(), car.GetTeamNum2(),
                          car.GetLoadoutBody());
            cvarManager->log(line);
        }
        char buf[420];
        std::snprintf(buf, sizeof buf,
                      "dbg t=%.2f %s replay=(%.0f,%.0f,%.0f)%s jeu=(%.0f,%.0f,%.0f) vit=(%.0f,%.0f,%.0f)/(%.0f,%.0f,%.0f) "
                      "rot=(%d,%d,%d)/(%d,%d,%d) ang=(%.2f,%.2f,%.2f)/(%.2f,%.2f,%.2f) sol=%d",
                      t - Start(), who.c_str(), s.pos.X, s.pos.Y, s.pos.Z, s.valid ? "" : "[absent]", a.X, a.Y, a.Z,
                      s.vel.X, s.vel.Y, s.vel.Z, v.X, v.Y, v.Z, s.rot.Pitch, s.rot.Yaw, s.rot.Roll, r.Pitch, r.Yaw, r.Roll,
                      s.ang.X, s.ang.Y, s.ang.Z, w.X, w.Y, w.Z, car.IsOnGround() ? 1 : 0);
        cvarManager->log(buf);
    };

    CarWrapper local = gameWrapper->GetLocalCar();   // null while demolished
    // your car replays the player you drive until you simulate; while you watch it waits under the field
    const bool localFollows = toi >= 0 && mode == MODE_PLAYBACK;
    if (!local.IsNull()) CarRules(local, toi >= 0);
    if (!local.IsNull()) Stash(local, toi < 0);
    if (debug && !local.IsNull()) {
        BoostWrapper boost = local.GetBoostComponent();
        if (!boost.IsNull()) {
            char buf[120];
            std::snprintf(buf, sizeof buf, "dbg boost ta voiture: %.0f%% illimite=%d demolition=%d", 100.f * boost.GetCurrentBoostAmount(),
                          boost.GetUnlimitedBoostRefCount(), local.GetDemolishTarget());
            cvarManager->log(buf);
        }
    }
    if (!local.IsNull() && localFollows) {
        const State s = at(CarPlayer());
        trace("toi(" + replay->names[CarPlayer()] + ")", local, s);
        if (s.valid) {
            gapSum += Len(Sub(local.GetLocation(), s.pos));
            gapN++;
        }
        if (cvarManager->getCvar("rlab_debug").getIntValue() != 2) burst = 0;
        else if (burst++ < 40) {
            const Vector a = local.GetLocation();
            WorldInfoWrapper world = sw.GetWorldInfo();
            char buf[220];
            std::snprintf(buf, sizeof buf, "pas monde=%.4f t=%.4f cible=(%.1f,%.1f,%.1f) jeu=(%.1f,%.1f,%.1f) vit=(%.0f,%.0f,%.0f)",
                          world.IsNull() ? 0.f : world.GetTimeSeconds(), t - Start(), s.pos.X, s.pos.Y, s.pos.Z, a.X, a.Y, a.Z,
                          s.vel.X, s.vel.Y, s.vel.Z);
            cvarManager->log(buf);
        }
        if (debug && gapN) {
            char buf[120];
            std::snprintf(buf, sizeof buf, "dbg recalage moyen de ta voiture: %.1f unites sur %d pas", gapSum / gapN, gapN);
            cvarManager->log(buf);
            gapSum = 0.f;
            gapN = 0;
        }
        const bool wasThere = CarPlayer() < static_cast<int>(lastValid.size()) && lastValid[CarPlayer()];
        if (CarPlayer() < static_cast<int>(lastValid.size())) lastValid[CarPlayer()] = s.valid;
        if (s.valid) ApplyCar(local, s);
        else if (wasThere && flowing) local.Demolish();
        else Freeze(local);
    } else if (debug) {
        cvarManager->log(local.IsNull() ? "dbg ta voiture: absente (demolie ?)"
                                        : toi < 0 ? "dbg ta voiture: attend sous le terrain (tu regardes)" : "dbg ta voiture: tu conduis");
    }
    ArrayWrapper<CarWrapper> cars = sw.GetCars();
    for (int i = 0; i < cars.Count(); i++) {   // a bot the game created again (after a reset) gets its player back
        CarWrapper car = cars.Get(i);
        if (BotIndex(car) == -1 && OurBot(car)) {
            MapBots(mode == MODE_SIM);
            break;
        }
    }
    // a demolished car leaves the game (a new one comes back 3 seconds later): in a simulation, its player was demolished
    std::vector<bool> onPitch(replay->names.size(), false);
    for (int i = 0; i < cars.Count(); i++) {
        CarWrapper car = cars.Get(i);
        if (car.IsNull() || (!local.IsNull() && Same(car, local))) continue;
        const int k = BotIndex(car);
        if (debug && k == -1) cvarManager->log("dbg voiture inconnue: " + NameOf(car));
        if (k == PARKED) Park(car);
        if (k < 0 || car.GetbHidden()) continue;
        onPitch[k] = true;
        CarRules(car, DrivesItself(k));
        // our name tag in the team's colour stands in for the game's (white on a bot)
        PrimitiveComponentWrapper plate = car.GetNameplateMeshComponent();
        const unsigned long hidePlate = cvarManager->getCvar("rlab_noms").getBoolValue() ? 1 : 0;
        if (!plate.IsNull() && plate.GetHiddenGame() != hidePlate) plate.SetHiddenGame(hidePlate);
        const State s = at(k);
        trace(BotName(k), car, s);
        auto painted = paintedAt.find(car.memory_address);
        // a new car; a blue one again now and then (an orange one is painted by the game, once)
        if (painted == paintedAt.end() || (GameTeam(k) == 0 && applyTicks - painted->second > 240)) {
            PaintCar(car, k);
            paintedAt[car.memory_address] = applyTicks;
        }
        const bool wasThere = k < static_cast<int>(lastValid.size()) && lastValid[k];
        if (k < static_cast<int>(lastValid.size())) lastValid[k] = s.valid;
        if (mode == MODE_SIM && RoleOf(k) != ROLE_GHOST) continue;   // bots and free cars play on their own
        if (mode == MODE_SIM && match == MATCH_OVER) continue;       // after the final whistle the ghosts coast too
        if (s.valid && mode == MODE_SIM) TrackCar(car, s);
        else if (s.valid) ApplyCar(car, s);
        else if (wasThere && flowing) {   // demolished at that moment of the replay: it blows up, and comes back
            car.Demolish();
            cvarManager->log("Replay Lab: " + replay->names[k] + " demoli dans le replay");
        }
        else Park(car);   // not on the pitch at that moment (or removed, in a scenario)
    }

    for (int k = 0; k < replay->Players() && mode == MODE_SIM; k++)
        if (!onPitch[k] && k != CarPlayer() && NeedsBot(k, true)) MarkDemolished(k);
    if (mode == MODE_SIM && ++nextoTick >= nexto::TICK_SKIP) {
        nextoTick = 0;
        for (int k = 0; k < replay->Players(); k++)
            if (RoleOf(k) == ROLE_NEXTO) {
                const auto t0 = std::chrono::steady_clock::now();
                NextoDecide(sw, local);
                if (debug) {
                    char buf[160];
                    std::snprintf(buf, sizeof buf, "dbg Nexto: %.2f ms, %d plots connus", std::chrono::duration<float, std::milli>(
                                  std::chrono::steady_clock::now() - t0).count(), static_cast<int>(pads.size()));
                    cvarManager->log(buf);
                }
                break;
            }
    }

    BallWrapper ball = sw.GetBall();
    if (ball.IsNull()) return;
    const State real = at(-1);
    bool follow = mode == MODE_PLAYBACK;
    if (mode == MODE_SIM && ballLocked) {
        bool release = !real.valid;
        for (int k = 0; k < replay->Players() && !release; k++) {
            if (RoleOf(k) == ROLE_GHOST) continue;
            const State r = StateAt(k, t);                                // where the real car was
            if (r.valid && Len(Sub(r.pos, real.pos)) < RELEASE_DIST) release = true;
        }
        if (!release && toi >= 0 && !local.IsNull() && Len(Sub(local.GetLocation(), real.pos)) < RELEASE_DIST) release = true;
        for (int i = 0; i < cars.Count() && !release; i++) {             // where the changed cars are now
            CarWrapper c = cars.Get(i);
            if (c.IsNull()) continue;
            const int k = BotIndex(c);
            if (k >= 0 && DrivesItself(k) && Len(Sub(c.GetLocation(), real.pos)) < RELEASE_DIST)
                release = true;
        }
        if (release) {
            ballLocked = false;
            releaseT = simT;
            cvarManager->log("Replay Lab: le ballon ne suit plus le replay a +" + std::to_string(simT).substr(0, 5) + " s" +
                             (real.valid ? " (une voiture changee s'en approche)" : " (le replay ne l'a plus)"));
        } else {
            follow = true;
        }
    }
    if (mode == MODE_SIM) UpdateMatch(ball);
    if (mode == MODE_SIM && match == MATCH_OVER) {   // the ball stays where the match ended
        ball.SetLocation(ballEnd);
        ball.SetVelocity(Vector(0, 0, 0));
        ball.SetAngularVelocity(Vector(0, 0, 0), false);
        follow = false;
    }
    if (debug) {
        const Vector a = ball.GetLocation();
        char buf[200];
        std::snprintf(buf, sizeof buf, "dbg t=%.2f ballon replay=(%.0f,%.0f,%.0f) jeu=(%.0f,%.0f,%.0f) %s", t - Start(),
                      real.pos.X, real.pos.Y, real.pos.Z, a.X, a.Y, a.Z, follow ? "suit le replay" : "libre");
        cvarManager->log(buf);
    }
    if (follow && real.valid) {
        ball.SetLocation(real.pos);
        ball.SetRotation(real.rot);
        ball.SetVelocity(real.vel);
        ball.SetAngularVelocity(real.ang, false);
    }
    if (mode == MODE_SIM) RecordFrame(sw);
}

/* ================================================================== helpers */

int ReplayLab::FrameAt(float t) const {
    if (!replay || replay->time.empty()) return 0;
    auto it = std::upper_bound(replay->time.begin(), replay->time.end(), t);
    return std::max(static_cast<int>(it - replay->time.begin()) - 1, 0);
}

State ReplayLab::StateAt(int k, float t) const {
    State s;
    if (!replay || replay->time.empty()) return s;
    const ReplayData& R = *replay;
    const int n = R.Frames();
    const int i = FrameAt(t), j = std::min(i + 1, n - 1);
    const size_t P = static_cast<size_t>(R.Players());
    const RBody& a = k < 0 ? R.ball[i] : R.cars[i * P + k];
    const RBody& b0 = k < 0 ? R.ball[j] : R.cars[j * P + k];
    if (!a.valid) return s;
    const RBody& b = b0.valid ? b0 : a;
    const float span = R.time[j] - R.time[i];
    const float f = span > 1e-4f ? std::min(std::max((t - R.time[i]) / span, 0.f), 1.f) : 0.f;
    if (span > 1e-4f && Len(Sub(b.vel, a.vel)) < 500.f) {
        // a smooth curve through both frames along their velocities (cubic Hermite)
        const float f2 = f * f, f3 = f2 * f;
        const float h00 = 2 * f3 - 3 * f2 + 1, h10 = f3 - 2 * f2 + f, h01 = 3 * f2 - 2 * f3, h11 = f3 - f2;
        s.pos = Add(Add(Mul(a.pos, h00), Mul(a.vel, h10 * span)), Add(Mul(b.pos, h01), Mul(b.vel, h11 * span)));
    } else {
        s.pos = Lerp(a.pos, b.pos, f);   // a touch, a bounce or a dodge in between: a straight line
    }
    s.vel = Lerp(a.vel, b.vel, f);
    s.ang = Lerp(a.ang, b.ang, f);
    float q[4];
    Nlerp(a.quat, b.quat, f, q);
    s.rot = ToRotator(q);
    if (k >= 0) {
        s.boost = R.boost[i * P + k] / 255.f;
        s.boosting = R.boosting[i * P + k] != 0;
    }
    s.valid = true;
    if (mirrored) {   // 180 degree turn around the vertical axis
        s.pos = Vector(-s.pos.X, -s.pos.Y, s.pos.Z);
        s.vel = Vector(-s.vel.X, -s.vel.Y, s.vel.Z);
        s.ang = Vector(-s.ang.X, -s.ang.Y, s.ang.Z);
        s.rot.Yaw = s.rot.Yaw + 32768;
        if (s.rot.Yaw > 32767) s.rot.Yaw -= 65536;
    }
    return s;
}

std::string ReplayLab::BallText() const {
    if (mode == MODE_SIM && waiting) return "en attente: bouge ta manette pour lancer";
    if (goalTeam >= 0) {
        char buf[80];
        std::snprintf(buf, sizeof buf, "BUT %s a +%.1f s", goalTeam == 0 ? "bleu" : "orange", goalT);
        return buf;
    }
    return ballLocked ? "ballon = replay" : "ballon simule";
}

std::string ReplayLab::ClockAt(float t) const {
    if (!replay) return "";
    const int i = FrameAt(t);
    const int s = replay->secs[i];
    char buf[32];
    std::snprintf(buf, sizeof buf, "%s%d:%02d", replay->otFrame >= 0 && i > replay->otFrame ? "+" : "", s / 60, s % 60);
    return buf;
}

// The clock shows s from the moment it turns s until it turns s - 1 (s + 1 in overtime); it stands still at kickoffs.
float ReplayLab::RemainingAt(float t) const {
    if (!replay || replay->secs.empty()) return 0.f;
    const int f = FrameAt(t), s = replay->secs[f];
    for (int h = f + 1; h < replay->Frames(); h++) {
        const int next = replay->secs[h];
        if (next == s) continue;
        const float left = replay->time[h] - t;
        if (next < s) return std::min(static_cast<float>(s), s - 1 + left);
        return std::max(static_cast<float>(s), s + 1 - left);
    }
    return static_cast<float>(s);
}

bool ReplayLab::OvertimeAt(float t) const {
    if (!replay || replay->otFrame < 0) return false;
    const int f = FrameAt(t);
    return f > replay->otFrame && replay->secs[f] > 0;
}

void ReplayLab::ScoreAt(float t, int score[2]) const {
    score[0] = score[1] = 0;
    if (!replay) return;
    const bool sim = mode == MODE_SIM;
    const int f = FrameAt(sim ? forkT : t);
    score[0] = replay->baseScore[0];
    score[1] = replay->baseScore[1];
    for (const auto& g : replay->goals)
        if (g.first <= f && g.second >= 0 && g.second < 2) score[g.second]++;
    if (sim && goalTeam >= 0) score[goalTeam]++;
}

float ReplayLab::SimClock() const {
    const float t = clockStop >= 0.f ? clockStop : simT;
    return inOvertime ? t - overtimeT : std::max(0.f, clockAtFork - t);
}

std::string ReplayLab::SimClockText() const {
    const float c = SimClock();
    const int s = inOvertime ? static_cast<int>(c) : static_cast<int>(std::ceil(c - 1e-4f));
    char buf[32];
    std::snprintf(buf, sizeof buf, "%s%d:%02d", inOvertime ? "+" : "", s / 60, s % 60);
    return buf;
}

// The simulation's match, by the game's rules: a goal stops the clock; at 0:00 the match ends when the ball touches the
// ground, and a goal scored before that still counts (the zero second goal); a tie then goes to overtime, where the
// next goal wins. Nothing counts any more once the match is over.
void ReplayLab::UpdateMatch(BallWrapper ball) {
    if (match == MATCH_OVER) return;
    const Vector p = ball.GetLocation();
    int score[2];
    auto result = [&] {
        ScoreAt(Now(), score);
        if (score[0] == score[1]) return "egalite " + std::to_string(score[0]) + " - " + std::to_string(score[1]) + ", prolongation";
        return std::string(score[0] > score[1] ? "victoire des bleus " : "victoire des oranges ") + std::to_string(score[0]) + " - " +
               std::to_string(score[1]);
    };
    if (goalTeam < 0 && Scored(p, GameKind())) {   // +Y is orange's side, unless the scene is turned around
        if (match != MATCH_PLAYING) {
            char where[80];
            std::snprintf(where, sizeof where, " (ballon en %.0f, %.0f, %.0f)", p.X, p.Y, p.Z);
            cvarManager->log("Replay Lab: but a +" + std::to_string(simT).substr(0, 5) + " s" + where);
        }
        goalTeam = (p.Y > 0) != mirrored ? 0 : 1;
        goalT = simT;
        clockStop = simT;
        const std::string who = goalTeam == 0 ? "les bleus" : "les oranges";
        if (match == MATCH_ZERO) {
            EndMatch(ball, "But a 0 seconde pour " + who + " : il compte ! " + result());
        } else if (match == MATCH_OVERTIME) {
            EndMatch(ball, "But en or pour " + who + " ! " + result());
        } else {
            Status("BUT pour " + who + " ! F11 recommencer, F9 retour au replay.", 30.f);
            char where[80];
            std::snprintf(where, sizeof where, " (ballon en %.0f, %.0f, %.0f)", p.X, p.Y, p.Z);
            cvarManager->log("Replay Lab: but a +" + std::to_string(simT).substr(0, 5) + " s" + where);
        }
        return;
    }
    if (match == MATCH_PLAYING && goalTeam < 0 && SimClock() <= 0.f) {
        match = MATCH_ZERO;
        Status("0:00 ! Le match se termine des que le ballon touche le sol : un but avant compte encore.", 10.f);
    }
    if (match == MATCH_ZERO && p.Z < ball.GetRadius() + 8.f) {   // the ball on the floor
        clockStop = simT;
        EndMatch(ball, "Le ballon touche le sol a 0:00 : " + result());
    }
}

// Like the game at the final whistle (a tie too: overtime starts with a kickoff, which a replay cannot give): nobody drives
// any more, the cars coast through the ball, which stays where it was, and the result stays on screen until the next
// simulation. (The game does not hide the ball when asked.)
void ReplayLab::EndMatch(BallWrapper ball, const std::string& how) {
    match = MATCH_OVER;
    matchResult = how;
    ballLocked = false;
    ballEnd = ball.GetLocation();
    ball.SetbCollideActors(0);
    ball.SetbBlockActors(0);
    char where[80];
    std::snprintf(where, sizeof where, " (ballon en %.0f, %.0f, %.0f)", ballEnd.X, ballEnd.Y, ballEnd.Z);
    cvarManager->log("Replay Lab: fin du match a +" + std::to_string(simT).substr(0, 5) + " s" + where);
    Status("FIN DU MATCH. " + how + ". F11 (ou reset) recommencer, F9 retour au replay.", 30.f);
}

void ReplayLab::BallBack(ServerWrapper sw) {
    BallWrapper ball = sw.IsNull() ? BallWrapper(0) : sw.GetBall();
    if (ball.IsNull() || ball.GetbCollideActors()) return;
    ball.SetbCollideActors(1);
    ball.SetbBlockActors(1);
}

// Only the mouse over the window is kept from the game. Not the keyboard: once clicked, the window keeps the keyboard
// focus (you drive with a controller and never click elsewhere), and blocking it then swallowed every shortcut.
bool ReplayLab::ShouldBlockInput() {
    return ImGui::GetIO().WantCaptureMouse || ImGui::GetIO().WantTextInput;
}

/* ================================================================== in-game drawing */

// Like the game's: each team's score and the clock at the top of the screen (kept in cinema mode, as in a real match).
void ReplayLab::DrawScoreboard(CanvasWrapper& canvas) {
    int score[2];
    ScoreAt(Now(), score);
    const bool sim = mode == MODE_SIM;
    const std::string clock = sim ? SimClockText() : ClockAt(rt);
    const int cx = canvas.GetSize().X / 2, top = 16, side = 84, mid = 132, h = 50;
    auto box = [&](int x, int w, int r, int g, int b) {
        Color(canvas, r, g, b, 225);
        canvas.SetPosition(Vector2{x, top});
        canvas.FillBox(Vector2{w, h});
    };
    auto text = [&](const std::string& s, int x, int w, int y, float scale) {
        const Vector2F size = canvas.GetStringSize(s, scale, scale);
        canvas.SetPosition(Vector2F{x + (w - size.X) / 2.f, static_cast<float>(y)});
        canvas.DrawString(s, scale, scale, true);
    };
    box(cx - mid / 2 - side, side, 25, 85, 215);
    box(cx - mid / 2, mid, 14, 17, 26);
    box(cx + mid / 2, side, 235, 115, 20);
    Color(canvas, 255, 255, 255, 255);
    text(std::to_string(score[0]), cx - mid / 2 - side, side, top + 6, 3.f);
    text(clock, cx - mid / 2, mid, top + 6, 3.f);
    text(std::to_string(score[1]), cx + mid / 2, side, top + 6, 3.f);
    std::string under;
    if (sim && match == MATCH_ZERO) under = "0 SECONDE : le ballon doit toucher le sol";
    else if (sim && match == MATCH_OVER) under = "FIN DU MATCH";
    else if (sim && inOvertime) under = "PROLONGATION";
    else if (!sim && OvertimeAt(rt)) under = "PROLONGATION";
    if (sim && !inOvertime && match == MATCH_PLAYING && SimClock() < 10.f) {
        char buf[16];
        std::snprintf(buf, sizeof buf, "%.1f s", SimClock());
        under = buf;
    }
    if (!under.empty()) {
        Color(canvas, 255, 220, 60, 255);
        text(under, cx - 200, 400, top + h + 4, 1.4f);
    }
    if (sim && match == MATCH_OVER) {   // like the game's end of match banner
        const int y = canvas.GetSize().Y / 4, w = 900;
        Color(canvas, 10, 12, 20, 200);
        canvas.SetPosition(Vector2{cx - w / 2, y});
        canvas.FillBox(Vector2{w, 104});
        Color(canvas, 255, 255, 255, 255);
        text("FIN DU MATCH", cx - w / 2, w, y + 8, 4.f);
        Color(canvas, 255, 220, 60, 255);
        text(matchResult, cx - w / 2, w, y + 70, 1.6f);
    }
}

// Like the game's name plates (which stay white on a bot): the player's name on his team's colour, smaller farther
// away; not on the car the camera follows. Outside cinema mode it also says who drives a changed car.
void ReplayLab::DrawNameTags(CanvasWrapper& canvas) {
    CameraWrapper cam = gameWrapper->GetCamera();
    ServerWrapper sw = gameWrapper->GetGameEventAsServer();
    if (cam.IsNull() || sw.IsNull()) return;
    const Vector camLoc = cam.GetLocation();
    const Axes camAx = AxesOf(cam.GetRotation());
    const bool cinema = cvarManager->getCvar("rlab_cinema").getBoolValue();
    const uintptr_t followed = Followed().memory_address;
    ArrayWrapper<CarWrapper> cars = sw.GetCars();
    for (int i = 0; i < cars.Count(); i++) {
        CarWrapper car = cars.Get(i);
        if (car.IsNull() || car.GetbHidden() || car.memory_address == followed) continue;
        const int k = BotIndex(car);
        if (k < 0 || car.GetLocation().Z < -100.f) continue;   // not the cars waiting under the field
        const Vector top = Add(car.GetLocation(), Vector(0, 0, 110));
        const float ahead = Dot(Sub(top, camLoc), camAx.f);
        if (ahead < 50.f) continue;
        std::string text = replay->names[k];
        if (!cinema && mode == MODE_SIM && RoleOf(k) == ROLE_BOT) text += k < static_cast<int>(demoed.size()) && demoed[k] ? " (demoli, bot)" : " (bot)";
        if (!cinema && mode == MODE_SIM && RoleOf(k) == ROLE_FREE) text += " (libre)";
        if (!cinema && mode == MODE_SIM && RoleOf(k) == ROLE_NEXTO) text += " (Nexto)";
        const float scale = std::min(1.6f, std::max(0.8f, 1800.f / Len(Sub(top, camLoc))));
        const Vector2F size = canvas.GetStringSize(text, scale, scale);
        const Vector2 p = canvas.Project(top);
        const float pad = 6.f * scale, x = p.X - size.X / 2.f - pad, y = p.Y - size.Y - 2.f * pad;
        if (GameTeam(k) == 0) Color(canvas, 25, 90, 225, 215);
        else Color(canvas, 235, 115, 20, 215);
        canvas.SetPosition(Vector2F{x, y});
        canvas.FillBox(Vector2F{size.X + 2.f * pad, size.Y + 2.f * pad});
        Color(canvas, 255, 255, 255, 255);
        canvas.SetPosition(Vector2F{x + pad, y + pad});
        canvas.DrawString(text, scale, scale, false);
    }
}

void ReplayLab::DrawCaptions(CanvasWrapper& canvas) {
    const Vector2 screen = canvas.GetSize();
    auto centered = [&](const std::string& s, float scale, float y) {
        const Vector2F size = canvas.GetStringSize(s, scale, scale);
        canvas.SetPosition(Vector2F{(screen.X - size.X) / 2.f, y});
        canvas.DrawString(s, scale, scale, true);
    };
    if (!title[0].empty()) {   // a band across the screen
        const float y = screen.Y * 0.36f;
        Color(canvas, 8, 10, 18, 185);
        canvas.SetPosition(Vector2F{0.f, y});
        canvas.FillBox(Vector2F{static_cast<float>(screen.X), title[1].empty() ? 120.f : 175.f});
        Color(canvas, 255, 255, 255, 255);
        centered(title[0], 6.f, y + 14.f);
        Color(canvas, 255, 200, 90, 255);
        if (!title[1].empty()) centered(title[1], 2.f, y + 122.f);
    }
    if (!caption[0].empty()) {   // a lower third
        const float big = 2.2f, small = 1.5f;
        const Vector2F a = canvas.GetStringSize(caption[0], big, big);
        const Vector2F b = caption[1].empty() ? Vector2F{0.f, 0.f} : canvas.GetStringSize(caption[1], small, small);
        const float w = std::max(a.X, b.X) + 44.f, h = a.Y + (caption[1].empty() ? 0.f : b.Y + 6.f) + 26.f;
        const float x = 60.f, y = screen.Y - 170.f - h;
        Color(canvas, 8, 10, 18, 200);
        canvas.SetPosition(Vector2F{x, y});
        canvas.FillBox(Vector2F{w, h});
        Color(canvas, 255, 120, 200, 255);   // Replay Lab's pink, as the window's
        canvas.SetPosition(Vector2F{x, y});
        canvas.FillBox(Vector2F{8.f, h});
        Color(canvas, 255, 255, 255, 255);
        canvas.SetPosition(Vector2F{x + 26.f, y + 12.f});
        canvas.DrawString(caption[0], big, big, true);
        if (!caption[1].empty()) {
            Color(canvas, 210, 215, 230, 255);
            canvas.SetPosition(Vector2F{x + 26.f, y + 18.f + a.Y});
            canvas.DrawString(caption[1], small, small, true);
        }
    }
}

void ReplayLab::DrawHud(CanvasWrapper canvas) {
    if (!gameWrapper->IsInFreeplay() && !gameWrapper->IsInReplay()) return;
    std::lock_guard<std::recursive_mutex> lock(mtx);
    DrawCaptions(canvas);
    if (replay && mode != MODE_IDLE && gameWrapper->IsInFreeplay() && cvarManager->getCvar("rlab_noms").getBoolValue())
        DrawNameTags(canvas);
    if (replay && mode != MODE_IDLE && gameWrapper->IsInFreeplay() && cvarManager->getCvar("rlab_tableau").getBoolValue())
        DrawScoreboard(canvas);
    if (cvarManager->getCvar("rlab_cinema").getBoolValue() && gameWrapper->IsInFreeplay()) return;   // filming: nothing else of ours
    std::vector<std::string> lines;
    if (gameWrapper->IsInReplay()) {
        lines.push_back("Replay Lab   F5/F10 simuler depuis ce moment");
    } else {
        lines.push_back("Replay Lab   F3 fenetre   F4 vue   F9 lecture/pause   F5/F10 simuler d'ici   F11 recommencer   F7 camera");
        if (cvarManager->getCvar("rlab_manette").getBoolValue())
            lines.push_back("Manette   croix: haut pause, gauche camera, droite joueur, bas statut   Back: simuler d'ici   reset: recommencer");
        if (replay && mode == MODE_PLAYBACK)
            lines.push_back(std::string(replay->scenario ? "Scenario (ta simulation)  " : "") + (playing ? "Lecture " : "Pause ") +
                            ClockAt(rt) + "   " + CamText());
        if (replay && mode == MODE_SIM) {
            char buf[200];
            std::snprintf(buf, sizeof buf, "Simulation +%.1f s depuis %s   %s   %s", simT, ClockAt(forkT).c_str(), BallText().c_str(),
                          CamText().c_str());
            lines.push_back(buf);
        }
    }
    if (std::chrono::steady_clock::now() < statusUntil) lines.push_back(status);
    else if (loading) lines.push_back("Lecture du replay...");
    Color(canvas, 10, 14, 24, 170);
    canvas.SetPosition(Vector2{20, 180});
    canvas.FillBox(Vector2{820, 12 + 24 * static_cast<int>(lines.size())});
    for (size_t i = 0; i < lines.size(); i++) {
        if (i == 0) Color(canvas, 255, 255, 255, 255);
        else Color(canvas, 255, 180, 235, 255);
        canvas.SetPosition(Vector2{30, 188 + 24 * static_cast<int>(i)});
        canvas.DrawString(lines[i], i == 0 ? 1.1f : 1.f, i == 0 ? 1.1f : 1.f, true);
    }
    if (replay && mode != MODE_IDLE && gameWrapper->IsInFreeplay()) DrawWorld(canvas);
}

// Real player names above the cars, and during a simulation where the changed cars really went.
void ReplayLab::DrawWorld(CanvasWrapper& canvas) {
    CameraWrapper cam = gameWrapper->GetCamera();
    ServerWrapper sw = gameWrapper->GetGameEventAsServer();
    if (cam.IsNull() || sw.IsNull()) return;
    const Vector camLoc = cam.GetLocation();
    const Axes camAx = AxesOf(cam.GetRotation());
    auto visible = [&](const Vector& p) { return Dot(Sub(p, camLoc), camAx.f) > 50.f; };
    auto line = [&](const Vector& a, const Vector& b, float width) {
        if (visible(a) && visible(b)) canvas.DrawLine(canvas.Project(a), canvas.Project(b), width);
    };
    auto teamColor = [&](int k) {
        const int team = mirrored ? 1 - replay->teams[k] : replay->teams[k];
        if (team == 0) Color(canvas, 110, 165, 255, 255);
        else Color(canvas, 255, 165, 70, 255);
    };
    auto label = [&](const Vector& at, const std::string& text) {
        const Vector top = Add(at, Vector(0, 0, 90));
        if (!visible(top)) return;
        const Vector2 p = canvas.Project(top);
        canvas.SetPosition(Vector2{p.X - 4 * static_cast<int>(text.size()), p.Y - 14});
        canvas.DrawString(text, 1.f, 1.f, true);
    };

    if (mode != MODE_SIM || !cvarManager->getCvar("rlab_ghosts").getBoolValue()) return;

    const float t = forkT + simT;
    // what the ball really did over the next 6 seconds
    Color(canvas, 255, 255, 255, 140);
    State prev = StateAt(-1, forkT);
    for (float dt = 0.1f; dt <= 6.f; dt += 0.1f) {
        const State cur = StateAt(-1, forkT + dt);
        if (prev.valid && cur.valid) line(prev.pos, cur.pos, 2.f);
        prev = cur;
    }
    const State ghostBall = StateAt(-1, t);
    if (!ballLocked && ghostBall.valid && visible(ghostBall.pos)) {
        const Vector2 c = canvas.Project(ghostBall.pos);
        const Vector2 e = canvas.Project(Add(ghostBall.pos, Mul(camAx.r, 92.f)));
        const int r = std::max(4, std::abs(e.X - c.X));
        Color(canvas, 255, 255, 255, 220);
        canvas.SetPosition(Vector2{c.X - r, c.Y - r});
        canvas.DrawBox(Vector2{2 * r, 2 * r});
    }
    // where the changed cars really went (ghost-role cars are already on the pitch for real)
    for (int k = 0; k < replay->Players(); k++) {
        if (RoleOf(k) == ROLE_GHOST) continue;
        const State car = StateAt(k, t);
        if (!car.valid) continue;
        if (roles[k] == ROLE_TOI) Color(canvas, 255, 255, 255, 200);
        else teamColor(k);
        const Axes ax = AxesOf(car.rot);
        const Vector center = Add(car.pos, Add(Mul(ax.f, OFF_F), Mul(ax.u, OFF_U)));
        Vector corners[8];
        for (int i = 0; i < 8; i++)
            corners[i] = Add(center, Add(Mul(ax.f, (i & 1) ? HALF_L : -HALF_L),
                                         Add(Mul(ax.r, (i & 2) ? HALF_W : -HALF_W), Mul(ax.u, (i & 4) ? HALF_H : -HALF_H))));
        for (int i = 0; i < 8; i++)
            for (int bit = 1; bit < 8; bit <<= 1)
                if (!(i & bit)) line(corners[i], corners[i | bit], 2.f);
        label(car.pos, replay->names[k] + " (vrai)");
    }
}

/* ================================================================== window (F3) */

// Returns true when your own car now stands for another player (the cars must be set up again).
bool ReplayLab::SetRole(int k, int role) {
    const bool carChanged = role == ROLE_TOI || roles[k] == ROLE_TOI;
    if (role == ROLE_TOI)
        for (int& r : roles) if (r == ROLE_TOI) r = ROLE_GHOST;
    roles[k] = role;
    if (role == ROLE_REMOVED && k == camTarget) {   // the camera was on him: on to the next player
        for (int step = 1; step < replay->Players(); step++) {
            const int c = (k + step) % replay->Players();
            if (roles[c] != ROLE_REMOVED) {
                camTarget = c;
                break;
            }
        }
    }
    if (role == ROLE_TOI) camMode = CAM_GAME;   // behind your car
    return carChanged;
}

void ReplayLab::DrawRoles() {
    ImGui::TextUnformatted("Role de chaque voiture (s'applique a la prochaine simulation / F11):");
    bool carChanged = false;
    for (int k = 0; k < replay->Players(); k++) {
        ImGui::PushID(k);
        const ImVec4 col = replay->teams[k] == 0 ? ImVec4(0.4f, 0.6f, 1.f, 1.f) : ImVec4(1.f, 0.62f, 0.25f, 1.f);
        ImGui::TextColored(col, "%s%s", replay->names[k].c_str(), k == ToiIndex() ? "  [tu conduis]" : (k == camTarget ? "  [camera]" : ""));
        ImGui::SameLine(230);
        ImGui::SetNextItemWidth(260);
        int role = roles[k];
        if (ImGui::Combo("##role", &role, ROLE_LABELS, ROLE_COUNT) && SetRole(k, role)) carChanged = true;

        ImGui::PopID();
    }
    if (ImGui::Button("Tous en fantome")) for (int& r : roles) if (r != ROLE_TOI) r = ROLE_GHOST;
    ImGui::SameLine();
    const int myTeam = replay->teams[ToiIndex() >= 0 ? ToiIndex() : std::min(camTarget, replay->Players() - 1)];
    if (ImGui::Button("Adversaires en bots Psyonix"))
        for (int k = 0; k < replay->Players(); k++) if (roles[k] != ROLE_TOI && replay->teams[k] != myTeam) roles[k] = ROLE_BOT;
    ImGui::SameLine();
    if (ImGui::Button("Adversaires en Nexto"))
        for (int k = 0; k < replay->Players(); k++) if (roles[k] != ROLE_TOI && replay->teams[k] != myTeam) roles[k] = ROLE_NEXTO;
    ImGui::SameLine();
    if (ImGui::Button("Personne pour moi (je regarde)")) {
        for (int& r : roles) if (r == ROLE_TOI) r = ROLE_GHOST;
        carChanged = true;
    }
    if (ToiIndex() < 0)
        ImGui::TextDisabled("Personne n'est 'Toi': tu regardes, chaque joueur dans sa vraie voiture (F4: joueur suivant).");
    if (carChanged) {
        const bool sim = mode == MODE_SIM;
        gameWrapper->Execute([this, sim](GameWrapper*) {
            SetupCars([this, sim] { if (!sim) StartPlayback(rt, playing); }, sim);
        });
    }
}

void ReplayLab::DrawCamera() {
    static const char* MODES[CAM_COUNT] = {"Jeu", "Joueur", "Libre", "Automatique"};
    ImGui::TextUnformatted("Camera (F7):");
    for (int m = 0; m < CAM_COUNT; m++) {
        ImGui::SameLine();
        if (ImGui::RadioButton(MODES[m], camMode == m) && camMode != m) {
            camMode = (m + CAM_COUNT - 1) % CAM_COUNT;
            gameWrapper->Execute([this](GameWrapper*) { NextCamMode(); });
        }
    }
    if (camMode == CAM_PLAYER) {
        ImGui::SetNextItemWidth(200);
        if (ImGui::BeginCombo("##camtarget", replay->names[std::min(camTarget, replay->Players() - 1)].c_str())) {
            for (int k = 0; k < replay->Players(); k++)
                if (ImGui::Selectable(replay->names[k].c_str(), k == camTarget)) camTarget = k;
            ImGui::EndCombo();
        }
        ImGui::SameLine();
        ImGui::TextDisabled("F4: joueur suivant.");
    } else if (camMode == CAM_FREE) {
        ImGui::TextDisabled("Ta manette quand elle ne conduit pas: gachettes avancer, stick tourner/monter, boost vite. En revoyant un replay ou un scenario.");
    } else if (camMode == CAM_AUTO) {
        ImGui::TextDisabled("Suit le joueur le plus proche du ballon, camera ballon.");
    }
    if (ImGui::Button("Camera ballon (F8)")) gameWrapper->Execute([this](GameWrapper*) { ToggleBallCam(); });
    ImGui::SameLine();
    bool his = cvarManager->getCvar("rlab_cam_joueur").getBoolValue();
    if (ImGui::Checkbox("Vue joueur: ses reglages camera et sa camera ballon (comme le replay)", &his))
        cvarManager->getCvar("rlab_cam_joueur").setValue(his ? 1 : 0);
    bool cinema = cvarManager->getCvar("rlab_cinema").getBoolValue();
    if (ImGui::Checkbox("Mode cinema: rien de Replay Lab a l'ecran (pour filmer; F3 pour cette fenetre)", &cinema))
        cvarManager->getCvar("rlab_cinema").setValue(cinema ? 1 : 0);
}

void ReplayLab::DrawOptions() {
    bool wait = cvarManager->getCvar("rlab_attendre").getBoolValue();
    if (ImGui::Checkbox("Comme l'entrainement: quand tu conduis, tout attend que tu bouges ta manette", &wait))
        cvarManager->getCvar("rlab_attendre").setValue(wait ? 1 : 0);
    bool sync = cvarManager->getCvar("rlab_sync").getBoolValue();
    if (ImGui::Checkbox("Le ballon suit le replay tant qu'aucune voiture changee ne s'approche", &sync))
        cvarManager->getCvar("rlab_sync").setValue(sync ? 1 : 0);
    bool ghosts = cvarManager->getCvar("rlab_ghosts").getBoolValue();
    if (ImGui::Checkbox("Dessiner le vrai replay en fil de fer pendant la simulation", &ghosts))
        cvarManager->getCvar("rlab_ghosts").setValue(ghosts ? 1 : 0);
    bool mirror = cvarManager->getCvar("rlab_mirror").getBoolValue();
    if (ImGui::Checkbox("Si ta voiture est orange, tourner la scene (tu defends le but du bas)", &mirror))
        cvarManager->getCvar("rlab_mirror").setValue(mirror ? 1 : 0);
    float speed = cvarManager->getCvar("rlab_speed").getFloatValue();
    if (ImGui::SliderFloat("Vitesse du jeu en simulation", &speed, 0.1f, 1.f, "%.2f")) cvarManager->getCvar("rlab_speed").setValue(speed);
}

void ReplayLab::Render() {
    bool keepOpen = true;   // false once the window's close button is clicked (F3 goes through BakkesMod itself)
    if (!ImGui::Begin(GetMenuTitle().c_str(), &keepOpen, ImGuiWindowFlags_None)) {
        ImGui::End();
        return;
    }
    std::lock_guard<std::recursive_mutex> lock(mtx);

    // ---- replay list
    static int selected = -1;
    const char* preview = selected >= 0 && selected < static_cast<int>(library.size()) ? library[selected].label.c_str()
                                                                                       : (listing ? "lecture du dossier..." : "choisir un replay");
    ImGui::SetNextItemWidth(460);
    if (ImGui::BeginCombo("##replays", preview)) {
        for (int i = 0; i < static_cast<int>(library.size()); i++)
            if (ImGui::Selectable(library[i].label.c_str(), i == selected)) selected = i;
        ImGui::EndCombo();
    }
    ImGui::SameLine();
    if (ImGui::Button("Ouvrir") && selected >= 0 && selected < static_cast<int>(library.size())) {
        const std::string id = library[selected].id;
        gameWrapper->Execute([this, id](GameWrapper*) { OpenReplay(id, 0, false, ""); });
    }
    ImGui::SameLine();
    if (ImGui::Button("Actualiser")) gameWrapper->Execute([this](GameWrapper*) { RefreshList(); });
    if (std::chrono::steady_clock::now() < statusUntil) ImGui::TextWrapped("%s", status.c_str());

    if (!replay) {
        ImGui::TextWrapped(loading ? "Lecture du replay..."
                                   : "Choisis un replay et Ouvrir. Ou, dans le visionneur de replays du jeu, appuie sur F5 "
                                     "au moment voulu: le replay s'ouvre ici et la simulation demarre de ce moment.");
        ImGui::End();
        if (!keepOpen) gameWrapper->Execute([this](GameWrapper*) { cvarManager->executeCommand("togglemenu " + GetMenuName()); });
        return;
    }
    ImGui::Separator();

    // ---- transport
    const int fi = FrameAt(Now());
    int score[2];
    ScoreAt(Now(), score);
    const int s0 = score[0], s1 = score[1];
    ImGui::Text("%s   %s %s   bleu %d - %d orange   %s   vue: %s", replay->date.substr(0, 10).c_str(), replay->map.c_str(),
                GameKind() == KIND_SOCCAR ? "" : KIND_LABELS[GameKind()], s0, s1,
                (mode == MODE_SIM ? SimClockText() : ClockAt(replay->time[fi])).c_str(), CamText().c_str());
    if (mode == MODE_SIM) {
        ImGui::TextColored(ImVec4(1.f, 0.45f, 0.85f, 1.f), "SIMULATION  +%.1f s depuis %s  (%s)", simT, ClockAt(forkT).c_str(),
                           BallText().c_str());
        if (ImGui::Button("Recommencer (F11)")) gameWrapper->Execute([this](GameWrapper*) { SetupCars([this] { rt = forkT; Fork(); }, true); });
        ImGui::SameLine();
        if (ImGui::Button("Retour au replay (F9)")) gameWrapper->Execute([this](GameWrapper*) { BackToReplay(); });
        ImGui::SameLine();
        if (ImGui::Button("Revoir cette simulation")) gameWrapper->Execute([this](GameWrapper*) { WatchScenario(); });
    } else {
        if (ImGui::Button("-5 s")) gameWrapper->Execute([this](GameWrapper*) { Seek(rt - 5.f); });
        ImGui::SameLine();
        if (ImGui::Button(playing ? "Pause (F9)" : "Lecture (F9)")) gameWrapper->Execute([this](GameWrapper*) { TogglePlay(); });
        ImGui::SameLine();
        if (ImGui::Button("+5 s")) gameWrapper->Execute([this](GameWrapper*) { Seek(rt + 5.f); });
        ImGui::SameLine();
        int sp = 2;
        for (int i = 0; i < 4; i++) if (std::fabs(SPEEDS[i] - playSpeed) < 1e-3f) sp = i;
        ImGui::SetNextItemWidth(80);
        if (ImGui::Combo("##speed", &sp, SPEED_LABELS, 4)) playSpeed = SPEEDS[sp];
        ImGui::SameLine();
        if (ImGui::Button("Simuler d'ici (F5)")) gameWrapper->Execute([this](GameWrapper*) { Fork(); });
    }
    ImGui::SameLine();
    if (ImGui::Button("Vue suivante (F4)")) gameWrapper->Execute([this](GameWrapper*) { NextView(); });
    float pos = (mode == MODE_SIM ? forkT : rt) - Start();
    ImGui::SetNextItemWidth(-1);
    if (ImGui::SliderFloat("##time", &pos, 0.f, End() - Start(), "%.1f s")) {
        const float target = Start() + pos;
        gameWrapper->Execute([this, target](GameWrapper*) { Seek(target); });
    }
    {   // goals (team colour) and saves (white) on the slider
        const ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const float span = std::max(End() - Start(), 1e-3f);
        auto x = [&](int frame) { return a.x + (b.x - a.x) * (replay->time[frame] - Start()) / span; };
        for (const auto& g : replay->goals)
            dl->AddLine(ImVec2(x(g.first), a.y), ImVec2(x(g.first), b.y), g.second == 0 ? IM_COL32(80, 140, 255, 255) : IM_COL32(255, 150, 50, 255), 3.f);
        for (const auto& s : replay->saves)
            dl->AddLine(ImVec2(x(s.first), b.y - 6), ImVec2(x(s.first), b.y), IM_COL32(255, 255, 255, 255), 3.f);
    }
    ImGui::TextDisabled("Barres bleues/orange: buts.  Traits blancs: arrets.");
    if (replay->scenario) {
        ImGui::TextColored(ImVec4(1.f, 0.45f, 0.85f, 1.f), "SCENARIO: tu revois ta simulation comme un replay (F5: resimuler d'un moment)");
        if (ImGui::Button("Revenir au replay d'origine")) gameWrapper->Execute([this](GameWrapper*) { BackToOriginal(); });
    } else if (scenario && scenario->Frames() > 30 && mode != MODE_SIM) {
        if (ImGui::Button("Revoir la derniere simulation")) gameWrapper->Execute([this](GameWrapper*) { WatchScenario(); });
    }
    ImGui::Separator();
    DrawCamera();
    ImGui::Separator();

    DrawRoles();
    ImGui::Separator();

    // ---- saves
    ImGui::Text("Arrets (%d)", static_cast<int>(replay->saves.size()));
    for (size_t i = 0; i < replay->saves.size(); i++) {
        const int frame = replay->saves[i].first, k = replay->saves[i].second;
        if (k < 0 || k >= replay->Players()) continue;
        ImGui::PushID(2000 + static_cast<int>(i));
        const ImVec4 col = replay->teams[k] == 0 ? ImVec4(0.4f, 0.6f, 1.f, 1.f) : ImVec4(1.f, 0.62f, 0.25f, 1.f);
        ImGui::TextColored(col, "%s  %s", ClockAt(replay->time[frame]).c_str(), replay->names[k].c_str());
        ImGui::SameLine(230);
        const float t = replay->time[frame];
        if (ImGui::Button("Voir")) gameWrapper->Execute([this, t](GameWrapper*) { PlayFrom(t - 2.5f, true); });
        ImGui::SameLine();
        if (ImGui::Button("Prendre sa place")) {
            for (int& r : roles) r = ROLE_GHOST;
            roles[k] = ROLE_TOI;
            camMode = CAM_GAME;
            gameWrapper->Execute([this, t](GameWrapper*) { SetupCars([this, t] { StartPlayback(t - 1.5f, false); Fork(); }, true); });
        }
        ImGui::SameLine();
        if (ImGui::Button("Sans lui")) {
            for (int& r : roles) r = ROLE_GHOST;
            roles[k] = ROLE_REMOVED;
            // watch from a player of the other team (the shooter's side)
            for (int c = 0; c < replay->Players(); c++)
                if (replay->teams[c] != replay->teams[k]) { camTarget = c; break; }
            gameWrapper->Execute([this, t](GameWrapper*) { SetupCars([this, t] { StartPlayback(t - 1.5f, false); Fork(); }, true); });
        }
        ImGui::PopID();
    }
    ImGui::Separator();
    DrawOptions();
    ImGui::End();
    if (!keepOpen) gameWrapper->Execute([this](GameWrapper*) { cvarManager->executeCommand("togglemenu " + GetMenuName()); });
}

/* ================================================================== F2 > Plugins tab */

void ReplayLab::RenderSettings() {
    ImGui::TextWrapped("Replay Lab rejoue tes replays dans l'entrainement libre: a tout moment, F5 et tu reprends la main "
                       "(ou une autre voiture devient un bot, ou disparait), avec la vraie physique du jeu.");
    ImGui::TextUnformatted("F3 fenetre   F4 vue   F9 lecture/pause   F5/F10 simuler d'ici   F11 recommencer");
    bool pad = cvarManager->getCvar("rlab_manette").getBoolValue();
    if (ImGui::Checkbox("Manette: croix haut pause, gauche camera, droite joueur, bas statut; Back: simuler d'ici; reset: recommencer", &pad))
        cvarManager->getCvar("rlab_manette").setValue(pad ? 1 : 0);
    // opening a window while BakkesMod is drawing its own menu crashes it: queue it on the game thread
    if (ImGui::Button("Ouvrir la fenetre Replay Lab (F3)"))
        gameWrapper->Execute([this](GameWrapper*) { cvarManager->executeCommand("togglemenu " + GetMenuName()); });
    ImGui::Separator();
    DrawOptions();
}
