// Simulating from a moment of the replay: each car takes its role, the game's physics takes over, with a real match's
// rules (clock, the 0 second rule, overtime, demolitions, limited boost); Nexto plays in the plugin itself.
#include "ReplayLab.h"
#include "LabMath.h"
#include "Rules.h"

using namespace lab;

void ReplayLab::Fork() {
    std::lock_guard<std::recursive_mutex> lock(mtx);
    if (!replay || mode == MODE_IDLE || !carsReady) {
        Status("Ouvre d'abord un replay (F3).");
        return;
    }
    ServerWrapper sw = gameWrapper->GetGameEventAsServer();
    CarWrapper local = gameWrapper->GetLocalCar();
    if (sw.IsNull()) return;
    if (goalReplay.on) CancelGoalReplay();   // from a moment of a goal's replay: that moment
    // A kickoff starts with its countdown (3, 2, 1, GO!), everybody still until GO, like the game. During a goal or a
    // kickoff countdown the match is off: the simulation starts at the next kickoff. So it is before the match (a replay
    // can start with the game waiting for players, then the first countdown).
    const int f = FrameAt(rt);
    const int phase = PhaseAt(rt);
    int next = f;   // the first frame from here that is not "other"
    while (phase == PHASE_OTHER && next + 1 < static_cast<int>(replay->phase.size()) && replay->phase[next] == PHASE_OTHER) next++;
    const bool beforeMatch = phase == PHASE_OTHER && next < static_cast<int>(replay->phase.size()) && replay->phase[next] == PHASE_COUNTDOWN;
    const bool atKickoff = phase == PHASE_COUNTDOWN || phase == PHASE_GOAL || beforeMatch ||
                           (phase == PHASE_PLAYING && f > 0 && replay->phase[f - 1] == PHASE_COUNTDOWN);
    const float kickoffAt = atKickoff ? NextKickoff(rt) : rt;
    const bool kickoffFound = atKickoff && (kickoffAt != rt || phase == PHASE_PLAYING);   // none after the last goal
    rt = kickoffAt;
    const int toi = ToiIndex();
    mirrored = Turned();
    mode = MODE_SIM;
    simSource = replay;
    forkT = rt;
    simT = 0.f;
    playing = false;
    lastWorld = -1.f;
    ballLocked = *opt.sync;
    goalTeam = -1;
    clockAtFork = RemainingAt(forkT);
    inOvertime = OvertimeAt(forkT);
    overtimeT = inOvertime ? -clockAtFork : 0.f;
    clockStop = -1.f;
    match = inOvertime ? MATCH_OVERTIME : clockAtFork <= 0.f ? MATCH_ZERO : MATCH_PLAYING;
    matchResult.clear();
    BallBack(sw);
    demoed.assign(replay->names.size(), false);
    bumped.assign(replay->names.size(), false);
    replayScored = false;
    nextoAction.assign(replay->names.size(), nexto::Action{});
    inputsNow.assign(replay->names.size(), Sticks{});
    scenario = nullptr;   // a new recording (a branch) starts with this simulation
    nextoTick = nexto::TICK_SKIP - 1;
    lastTouch[0] = lastTouch[1] = -1;
    goalScorer = -1;
    goalKmh = 0.f;
    goalReplayed = false;
    // how it starts: at once, when you move (like custom training, when you drive), or after a countdown
    const int start = *opt.attendre;
    const bool kickoffStart = kickoffFound || start == 2;
    kickoff = kickoffStart ? KICKOFF_COUNTDOWN : 0.f;
    waiting = !kickoffStart && toi >= 0 && start == 1;

    PadsAtFork(sw);  // the boost pads as in the replay at that moment
    MapBots(true);   // removed players' bots are set aside
    PlaceAtFork(!waiting && !kickoffStart);
    if (kickoffStart) CountdownMessage(3);
    for (int k = 0; k < replay->Players(); k++) {   // who is in the air without his flip, for the log
        const Air a = AirAt(k, forkT);
        if (a.jumped || a.flipped)
            cvarManager->log("Replay Lab: au depart " + replay->names[k] + (a.flipped ? " a deja utilise son flip"
                             : a.sinceJump < DODGE_WINDOW ? " a saute (flip encore " + std::to_string(DODGE_WINDOW - a.sinceJump).substr(0, 4) + " s)"
                                                          : " a saute il y a plus de 1,25 s: plus de flip"));
    }
    if (match == MATCH_ZERO) Status("0:00 : le match se termine des que le ballon touche le sol, un but avant compte encore.", 8.f);
    cvarManager->executeCommand("sv_soccar_gamespeed " + std::to_string(*opt.speed), false);
    if (kickoffStart) Status(std::string(kickoffFound ? "Coup d'envoi" : "Depart") + ": 3, 2, 1, GO ! comme en match. F11 recommencer, F9 retour au replay.", 6.f);
    else if (waiting) Status("Simulation prete: elle demarre des que tu bouges ta manette, comme l'entrainement. F11 recommencer, F9 retour au replay.", 8.f);
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

void ReplayLab::OnSimulateKey() {
    if (gameWrapper->IsInReplay()) {
        OpenFromViewer();
        return;
    }
    std::lock_guard<std::recursive_mutex> lock(mtx);
    if (mode == MODE_PLAYBACK) Fork();
    else if (mode == MODE_SIM) Retry();
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
        goalReplayed = true;   // a goal's replay due meanwhile is not shown any more
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

int ReplayLab::RoleOf(int k) const {
    // a player demolished in the simulation no longer follows the replay: back in a new car, the Psyonix AI plays him
    // touched by a car that drives itself, too (Bumped), or past a goal of the replay the simulation did not score
    if (mode == MODE_SIM && roles[k] == ROLE_GHOST && (replayScored || (k < static_cast<int>(demoed.size()) && demoed[k]) ||
                                                       (k < static_cast<int>(bumped.size()) && bumped[k])))
        return ROLE_BOT;
    if (roles[k] == ROLE_NEXTO && !nextoModel.Loaded()) return ROLE_BOT;
    return roles[k];
}

bool ReplayLab::DrivesItself(int k) const {
    const int r = RoleOf(k);
    return r == ROLE_BOT || r == ROLE_FREE || r == ROLE_NEXTO || r == ROLE_TOI;   // you: in your car or his bot
}

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
    NotePad(pad);
    RecordPad(pad, true);
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
        padPlaces.clear();
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

std::string ReplayLab::BallText() const {
    if (mode == MODE_SIM && waiting) return "en attente: bouge ta manette pour lancer";
    if (goalTeam >= 0) {
        char buf[80];
        std::snprintf(buf, sizeof buf, "BUT %s a +%.1f s", goalTeam == 0 ? "bleu" : "orange", goalT);
        return buf;
    }
    return ballLocked ? "ballon = replay" : "ballon simule";
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
        goalScorer = SimGoalScorer(goalTeam);
        goalKmh = Kmh(ball.GetVelocity());
        ballLocked = false;
        ExplodeBall(gameWrapper->GetGameEventAsServer(), goalTeam, goalScorer, p);   // with the scorer's goal explosion
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
    if (sw.IsNull()) return;
    ShowBall(sw, true);   // after a goal: back (a new one if it blew up)
    BallWrapper ball = sw.GetBall();
    if (ball.IsNull()) {
        cvarManager->log("Replay Lab: pas de ballon, un nouveau vient");
        ballRenew = true;
        return;
    }
    if (ballRenew) return;   // the blown-up one stays hidden until the new one comes
    if (ball.GetbHidden()) {   // hidden by something else than us: shown all the same
        cvarManager->log("Replay Lab: le ballon etait cache, il revient");
        ball.SetHidden2(0);
    }
    if (ball.GetbCollideActors()) return;
    ball.SetbCollideActors(1);
    ball.SetbBlockActors(1);
}

// A pad seen picked up or respawning is one of this game's: kept with its place and its own respawn delay, to set it as
// in the replay.
void ReplayLab::NotePad(ActorWrapper pad) {
    ServerWrapper sw = gameWrapper->GetGameEventAsServer();
    if (pad.IsNull() || sw.IsNull()) return;
    if (sw.memory_address != padServer) {   // another game: those pads are gone
        padServer = sw.memory_address;
        pads.clear();
        padPlaces.clear();
    }
    if (padPlaces.count(pad.memory_address)) return;
    padPlaces[pad.memory_address] = {pad.GetLocation(), VehiclePickupWrapper(pad.memory_address).GetRespawnDelay(), {}};
    padsOf.reset();   // its events are not listed yet
}

// Each known pad's events in the replay that plays (the scene may be turned around), listed once per replay.
void ReplayLab::PadEvents() {
    if (padsOf.lock() == replay && padsMirrored == mirrored) return;
    padsOf = replay;
    padsMirrored = mirrored;
    for (auto& [address, pad] : padPlaces) {
        pad.events.clear();
        const float x = mirrored ? -pad.at.X : pad.at.X, y = mirrored ? -pad.at.Y : pad.at.Y;
        for (int e = 0; e < static_cast<int>(replay->pads.size()); e++) {
            const ReplayData::PadEvent& ev = replay->pads[e];
            if ((ev.x - x) * (ev.x - x) + (ev.y - y) * (ev.y - y) < 250.f * 250.f) pad.events.push_back(e);
        }
    }
}

// Whether the replay has this pad taken at t, and when it comes back: its own event, or its respawn delay.
bool ReplayLab::PadTaken(const KnownPad& pad, float t, float* back) const {
    int last = -1;   // its last event at t
    for (int e : pad.events) {
        if (replay->pads[e].time > t) break;
        last = e;
    }
    if (last < 0 || !replay->pads[last].taken) return false;
    float until = replay->pads[last].time + pad.delay;
    for (int e : pad.events)
        if (replay->pads[e].time > replay->pads[last].time && !replay->pads[e].taken) {
            until = replay->pads[e].time;
            break;
        }
    if (back) *back = until;
    return until > t;
}

// The boost pads as they were in the replay at the moment the simulation starts (a reset puts them back too): each pad
// with its own delay again, up, or taken if the replay had it taken and not back yet, back when it came back in the
// replay (held until then: the game's timer would bring it back after its delay from now). Each known pad is set by
// hand: the game's ResetPickups leaves in freeplay the pads the last try took.
void ReplayLab::PadsAtFork(ServerWrapper sw) {
    padReturns.clear();
    sw.ResetPickups();   // the pads not known yet
    if (!replay || sw.memory_address != padServer) return;
    PadEvents();
    for (auto& [address, pad] : padPlaces) {
        VehiclePickupWrapper p(address);
        p.SetRespawnDelay(pad.delay);
        float back = 0.f;
        if (!PadTaken(pad, forkT, &back)) {
            if (p.GetbPickedUp()) p.Respawn2();
            continue;
        }
        p.SetRespawnDelay(PAD_HELD);
        if (!p.GetbPickedUp()) p.SetPickedUp(1, CarWrapper(0));
        p.SetRespawnDelay(pad.delay);
        padReturns.push_back({address, back - forkT});
    }
}

// A try keeps its pads like a replay: taken and back, in the replay's coordinates (the scene may be turned around).
void ReplayLab::RecordPad(ActorWrapper pad, bool taken) {
    if (mode != MODE_SIM || !scenario || pad.IsNull()) return;
    const Vector at = pad.GetLocation();
    scenario->pads.push_back({mirrored ? -at.X : at.X, mirrored ? -at.Y : at.Y, Now(), taken});
}

void ReplayLab::UpdatePads() {
    for (auto it = padReturns.begin(); it != padReturns.end();) {
        if (simT < it->second) {
            ++it;
            continue;
        }
        VehiclePickupWrapper pad(it->first);
        if (pad.GetbPickedUp()) pad.Respawn2();
        it = padReturns.erase(it);
    }
}

// While you watch, the pads are the replay's at that moment: they follow it back and forth, and stand still when it is
// paused (the game's own timers are held off, they would bring a pad back after its delay even then). Tries record theirs.
void ReplayLab::SyncPads(float t) {
    if (gameWrapper->GetGameEventAsServer().memory_address != padServer) return;
    PadEvents();
    for (auto& [address, pad] : padPlaces) {
        VehiclePickupWrapper p(address);
        if (p.GetRespawnDelay() != PAD_HELD) p.SetRespawnDelay(PAD_HELD);
        const bool taken = PadTaken(pad, t, nullptr);
        if (taken == (p.GetbPickedUp() != 0)) continue;
        if (taken) p.SetPickedUp(1, CarWrapper(0));
        else p.Respawn2();
    }
}

// The pads' own respawn delays back (the plugin is unloaded or the replay closed).
void ReplayLab::PadDelaysBack() {
    ServerWrapper sw = gameWrapper->GetGameEventAsServer();
    if (sw.IsNull() || sw.memory_address != padServer) return;
    for (auto& [address, pad] : padPlaces) VehiclePickupWrapper(address).SetRespawnDelay(pad.delay);
}
