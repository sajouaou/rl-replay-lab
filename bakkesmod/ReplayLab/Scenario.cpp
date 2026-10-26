// Scenarios: while simulating, every physics tick records where the ball and each player's car are, so the
// simulation can be watched again like a replay (timeline, pause, slow motion, cameras), filmed, or simulated
// from again at any moment. A scenario is a ReplayData like a replay file, in the same time base.
#include "ReplayLab.h"
#include "LabMath.h"

using namespace lab;

namespace {
constexpr float MAX_SCENARIO = 300.f;   // seconds of simulation kept

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
    if (!scenario) {
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
    }
    ReplayData& s = *scenario;
    if (!s.time.empty() && (t <= s.time.back() || t - s.time.front() > MAX_SCENARIO)) return;
    BallWrapper ball = sw.GetBall();
    s.time.push_back(t);
    // the simulation's clock (in overtime counting up, from the frame it started)
    s.secs.push_back(inOvertime ? static_cast<int>(SimClock()) : static_cast<int>(std::ceil(SimClock() - 1e-4f)));
    if (inOvertime && s.otFrame < 0) s.otFrame = s.Frames() - 2;
    s.ball.push_back(ball.IsNull() ? RBody{} : BodyOf(ball, mirrored));
    for (int k = 0; k < s.Players(); k++) {
        CarWrapper car = CarOf(k);
        BoostWrapper boost = car.IsNull() ? BoostWrapper(0) : car.GetBoostComponent();
        s.cars.push_back(car.IsNull() ? RBody{} : BodyOf(car, mirrored));
        s.boost.push_back(static_cast<unsigned char>(
            boost.IsNull() ? 0 : std::lround(255.f * std::min(std::max(boost.GetCurrentBoostAmount(), 0.f), 1.f))));
        s.boosting.push_back(k < static_cast<int>(boostingNow.size()) ? boostingNow[k] : 0);
        unsigned char moves = 0;   // jumping, double jumping, dodging: what the car has left in the air when simulated again
        if (!car.IsNull()) {
            if (!car.GetJumpComponent().IsNull() && car.GetJumpComponent().GetbActive()) moves |= 1;
            if (!car.GetDoubleJumpComponent().IsNull() && car.GetDoubleJumpComponent().GetbActive()) moves |= 2;
            if (!car.GetDodgeComponent().IsNull() && car.GetDodgeComponent().GetbActive()) moves |= 4;
        }
        s.moves.push_back(moves);
    }
    if (goalTeam >= 0 && s.goals.empty()) s.goals.push_back({s.Frames() - 1, goalTeam});
}

void ReplayLab::WatchScenario() {
    std::lock_guard<std::recursive_mutex> lock(mtx);
    if (!scenario || scenario->Frames() < 30) {
        Status("Rien a revoir: lance d'abord une simulation (F5).");
        return;
    }
    if (!original) original = replay;
    replay = scenario;
    const float start = scenario->time.front();
    PlayFrom(start, true);
    Status("Tu revois ta simulation comme un replay. F9 pause, F7 cameras, F5 resimuler d'un moment, fenetre F3: replay d'origine.", 10.f);
}

void ReplayLab::BackToOriginal() {
    std::lock_guard<std::recursive_mutex> lock(mtx);
    if (!original || replay == original) return;
    const float t = replay->time.empty() ? Start() : replay->time.front();
    replay = original;
    PlayFrom(t, false);
    Status("Retour au replay d'origine, au moment ou la simulation commencait.");
}
