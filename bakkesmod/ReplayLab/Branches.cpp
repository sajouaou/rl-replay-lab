// Branches: while simulating, the ball and each player's car are recorded 60 times a second, so the simulation can be
// watched again like a replay (timeline, pause, slow motion, cameras, its goals blown up and replayed), filmed, or
// simulated from again at any moment. Tries are not kept: the last one is there until the next, and becomes a branch
// only when you save it (d-pad down held, rlab_sauver); then it stays (the last few saved). A branch is a ReplayData like
// a replay file, in the same time base.
#include "ReplayLab.h"
#include "LabMath.h"

using namespace lab;

namespace {
constexpr float MAX_BRANCH = 300.f;     // seconds of simulation kept in a branch
constexpr float STEP = 1.f / 60.f;      // recorded 60 times a second (the game's physics runs at 120): smooth, half the memory
constexpr size_t MAX_BRANCHES = 8;      // saved branches kept, the oldest dropped

// an actor's rigid body as a replay would record it (turned back around if the scene is turned around)
RBody BodyOf(ActorWrapper actor, bool mirrored) {
    RBody b;
    b.pos = actor.GetLocation();
    b.vel = actor.GetVelocity();
    b.ang = actor.GetAngularVelocity();
    Rotator rot = actor.GetRotation();
    if (mirrored) {
        b.pos = Vector(-b.pos.X, -b.pos.Y, b.pos.Z);
        b.vel = Vector(-b.vel.X, -b.vel.Y, b.vel.Z);
        b.ang = Vector(-b.ang.X, -b.ang.Y, b.ang.Z);
        rot.Yaw += 32768;
    }
    ToQuat(rot, b.quat);
    b.valid = true;
    return b;
}
}  // namespace

void ReplayLab::RecordFrame(ServerWrapper sw) {
    if (!replay || mode != MODE_SIM) return;
    const float t = Now();
    if (!scenario) {   // a new try, from where the simulation started (the last unsaved one goes)
        scenario = std::make_shared<ReplayData>();
        ReplayData& s = *scenario;
        s.id = replay->id;   // same id: the bots keep the look they wear
        s.map = replay->map;
        s.date = replay->date;
        s.names = replay->names;
        s.teams = replay->teams;
        s.looks = replay->looks;
        s.cams = replay->cams;
        s.ballArch = replay->ballArch;
        int score[2];
        ScoreAt(forkT, score);   // without its goal: none yet
        s.baseScore[0] = score[0];
        s.baseScore[1] = score[1];
        s.scenario = true;
        for (const ReplayData::PadEvent& e : replay->pads)   // the pads as they were when it started; then its own
            if (e.time <= forkT) s.pads.push_back(e);
        // "depuis 2:41 de la Branche 2"; its number comes when it is saved
        const bool fromSaved = replay->scenario && replay->label.compare(0, 8, "Branche ") == 0;
        s.label = "depuis " + ClockAt(forkT) + (fromSaved ? " de la " + replay->label.substr(0, replay->label.find(" depuis"))
                                                           : replay->scenario ? " d'une simulation" : "");
    }
    ReplayData& s = *scenario;
    if (!s.time.empty() && (t < s.time.back() + STEP * 0.99f || t - s.time.front() > MAX_BRANCH)) return;
    BallWrapper ball = sw.GetBall();
    s.time.push_back(t);
    // the simulation's clock (in overtime counting up, from the frame it started)
    s.secs.push_back(inOvertime ? static_cast<int>(SimClock()) : static_cast<int>(std::ceil(SimClock() - 1e-4f)));
    if (inOvertime && s.otFrame < 0) s.otFrame = s.Frames() - 2;
    s.ball.push_back(ball.IsNull() || ballGone ? RBody{} : BodyOf(ball, mirrored));
    for (int k = 0; k < s.Players(); k++) {
        CarWrapper car = CarOf(k);
        BoostWrapper boost = car.IsNull() ? BoostWrapper(0) : car.GetBoostComponent();
        s.cars.push_back(car.IsNull() ? RBody{} : BodyOf(car, mirrored));
        s.boost.push_back(static_cast<unsigned char>(
            boost.IsNull() ? 0 : std::lround(255.f * std::min(std::max(boost.GetCurrentBoostAmount(), 0.f), 1.f))));
        const Sticks in = k < static_cast<int>(inputsNow.size()) ? inputsNow[k] : Sticks{};
        s.boosting.push_back(in.boosting);
        s.throttle.push_back(in.throttle);
        s.steer.push_back(in.steer);
        // jumping, double jumping, dodging (what the car has left in the air when simulated again), powersliding
        unsigned char moves = in.slide ? 8 : 0;
        if (!car.IsNull()) {
            if (!car.GetJumpComponent().IsNull() && car.GetJumpComponent().GetbActive()) moves |= 1;
            if (!car.GetDoubleJumpComponent().IsNull() && car.GetDoubleJumpComponent().GetbActive()) moves |= 2;
            if (!car.GetDodgeComponent().IsNull() && car.GetDodgeComponent().GetbActive()) moves |= 4;
        }
        s.moves.push_back(moves);
    }
    if (goalTeam >= 0 && s.goals.empty()) {   // its goal, who scored it
        s.goals.push_back({s.Frames() - 1, goalTeam});
        s.scorers.push_back({goalScorer, -1});
        s.label += std::string(" - but ") + (goalTeam == 0 ? "bleu" : "orange") + (goalScorer >= 0 ? " de " + PlayerText(goalScorer) : "");
    }
}

// The last try becomes a branch, kept.
void ReplayLab::SaveBranch() {
    std::lock_guard<std::recursive_mutex> lock(mtx);
    if (!scenario || scenario->Frames() < 30) {
        Status("Rien a sauvegarder: simule d'abord (F5 / Back).");
        return;
    }
    if (std::find(branches.begin(), branches.end(), scenario) != branches.end()) {
        Status(scenario->label.substr(0, scenario->label.find(" depuis")) + " est deja sauvegardee.", 4.f);
        return;
    }
    scenario->label = "Branche " + std::to_string(++branchCount) + " " + scenario->label;
    branches.push_back(scenario);
    if (branches.size() > MAX_BRANCHES) branches.erase(branches.begin());
    Status("Sauvegardee: " + scenario->label + ". Croix haut longue: la revoir.", 6.f);
}

// Branch i, or (-1) the last try, saved or not.
void ReplayLab::WatchBranch(int i) {
    std::lock_guard<std::recursive_mutex> lock(mtx);
    const std::shared_ptr<ReplayData> branch = i < 0 ? scenario : i < static_cast<int>(branches.size()) ? branches[i] : nullptr;
    if (!branch || branch->Frames() < 30) {
        Status("Rien a revoir: lance d'abord une simulation (F5 / Back).");
        return;
    }
    CancelGoalReplay();
    replay = branch;
    mode = MODE_PLAYBACK;   // still while its cars are set up
    playing = false;
    rt = branch->time.front();
    PlayFrom(rt, true);
    Status((branch->label.compare(0, 8, "Branche ") == 0 ? branch->label : "Ta simulation " + branch->label) +
           ": tu la revois comme un replay. F9 pause, F7 cameras, F5 resimuler d'un moment, croix bas longue: sauvegarder.", 10.f);
}

// D-pad up held: the original replay, then each branch from the last one back to the first, and around again.
void ReplayLab::NextBranch() {
    std::lock_guard<std::recursive_mutex> lock(mtx);
    if (!replay || mode == MODE_IDLE) return;
    if (branches.empty()) {
        Status("Pas encore de branche sauvegardee: simule (Back), puis croix bas longue pour sauvegarder.");
        return;
    }
    int at = -1;   // the one on screen (-1: the replay)
    for (size_t b = 0; b < branches.size(); b++)
        if (branches[b] == replay) at = static_cast<int>(b);
    if (at == 0) BackToOriginal();
    else WatchBranch(at < 0 ? static_cast<int>(branches.size()) - 1 : at - 1);
}

void ReplayLab::BackToOriginal() {
    std::lock_guard<std::recursive_mutex> lock(mtx);
    if (!original) return;
    if (replay == original) {   // a simulation of it: back to the replay
        if (mode == MODE_SIM) BackToReplay();
        return;
    }
    CancelGoalReplay();
    const float t = replay->time.empty() ? Start() : replay->time.front();
    replay = original;
    PlayFrom(t, false);
    Status("Retour au replay d'origine, au moment ou la branche commencait.");
}

// F11, the reset button: the last simulation again, from the same moment of what it was started from (the replay or a
// branch); none yet: one from here.
void ReplayLab::Retry() {
    std::lock_guard<std::recursive_mutex> lock(mtx);
    if (!replay || mode == MODE_IDLE) return;
    CancelGoalReplay();
    goalReplayed = true;   // its goal's replay, due meanwhile, is not shown any more
    if (!simSource) {
        SetupCars([this] { Fork(); }, true);
        return;
    }
    replay = simSource;
    SetupCars([this] {
        rt = forkT;
        Fork();
    }, true);
}
