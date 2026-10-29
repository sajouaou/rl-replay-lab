// The replay played like the match it was, as the game shows it:
//   kickoff      the countdown (3, 2, 1, GO!) with the cars on their spots
//   goal         the ball blows up with the scorer's own goal explosion, his name and the ball's speed show, the cars
//                play on for 3 seconds; then the game showed the goal again (not recorded in the file: a hole in time),
//                so the plugin shows it again from the recorded frames, or jumps straight to the kickoff
//   demolition   the car blows up when it did in the replay and is back when it was
// The screen goes dark around each jump in time. A simulation's goal blows up too, with the goal explosion of the last
// player of the scoring team to touch the ball, and can be shown again the same way (rlab_revoir_simulation).
#include "ReplayLab.h"
#include "LabMath.h"
#include "Draw.h"

using namespace lab;

namespace {
constexpr float FADE = 0.3f;             // the screen goes dark this long before a jump in time, and clears as long after
constexpr float BANNER = 3.f;            // who scored stays this long
constexpr float GOAL_LEAD = 6.f;         // a goal's replay starts this long before the goal...
constexpr float GOAL_TAIL = 2.5f;        // ...and ends this long after it
constexpr float SIM_AFTER_GOAL = 2.5f;   // a simulation plays on this long after its goal before showing it again
}  // namespace

/* ================================================================== the match's timeline */

int ReplayLab::PhaseAt(float t) const {
    return replay && !replay->phase.empty() ? replay->phase[FrameAt(t)] : PHASE_OTHER;
}

// The jump from a goal's phase to its kickoff (found at load: Decode): the game showed the goal there.
bool ReplayLab::HoleAfter(int frame) const {
    return std::binary_search(replay->cuts.begin(), replay->cuts.end(), frame);
}

int ReplayLab::GoalBefore(int frame) const {
    int goal = -1;
    for (size_t g = 0; g < replay->goals.size(); g++)
        if (replay->goals[g].first <= frame) goal = static_cast<int>(g);
    return goal;
}

float ReplayLab::NextKickoff(float t) const {
    if (!replay || replay->phase.empty()) return t;
    for (int i = FrameAt(t); i < replay->Frames(); i++)
        if (replay->phase[i] == PHASE_PLAYING) return replay->time[i];
    return t;
}

// From a goal's explosion to the next kickoff (in a scenario: from its goal on).
bool ReplayLab::BallGoneAt(float t) const {
    if (!replay) return false;
    const int f = FrameAt(t);
    if (!replay->phase.empty()) return replay->phase[f] == PHASE_GOAL;
    return GoalBefore(f) >= 0;
}

float ReplayLab::FadeAt(float t) const {
    float dark = 0.f;
    if (replay && !replay->phase.empty()) {   // a hole in time coming (darker as it nears), or just passed
        const ReplayData& R = *replay;
        const int f = FrameAt(t);
        for (int i = f; i + 1 < R.Frames() && R.time[i] < t + FADE; i++)
            if (HoleAfter(i)) {
                dark = std::max(dark, 1.f - (R.time[i] - t) / FADE);
                break;
            }
        for (int i = f; i > 0 && t - R.time[i] < FADE; i--)
            if (HoleAfter(i - 1)) {
                dark = std::max(dark, 1.f - (t - R.time[i]) / FADE);
                break;
            }
    }
    if (goalReplay.on) {   // a goal's replay starting, or ending
        dark = std::max(dark, 1.f - (t - goalReplay.from) / FADE);
        dark = std::max(dark, 1.f - (goalReplay.to - t) / FADE);
    }
    return std::min(std::max(dark, 0.f), 1.f);
}

float ReplayLab::ShownT() const { return goalReplay.on ? goalReplay.matchT : Now(); }

// Playback time goes on; at the end of what was recorded after a goal, the goal is shown again (or the kickoff comes
// straight away), then the kickoff.
void ReplayLab::AdvancePlayback(float dt) {
    const float next = rt + dt * playSpeed;
    if (goalReplay.on) {
        rt = std::min(next, goalReplay.to);
        if (next >= goalReplay.to) EndGoalReplay();
        return;
    }
    const ReplayData& R = *replay;
    for (int i = FrameAt(rt); i + 1 < R.Frames() && R.time[i] < next; i++) {
        if (!HoleAfter(i)) continue;
        const int goal = GoalBefore(i);
        if (goal >= 0 && *opt.revoirButs) StartGoalReplay(goal, R.time[i + 1], R.time[i]);
        else rt = R.time[i + 1];
        return;
    }
    rt = next;
    if (rt >= End()) {
        rt = End();
        playing = false;
    }
}

/* ================================================================== a goal shown again */

// Like the game's replay of a goal: from a few seconds before it, the camera on the scorer with ball cam (unless you
// film with the free camera), REPLAY on screen; the scoreboard shows the match as it stands after the goal.
void ReplayLab::StartGoalReplay(int goal, float resume, float matchT) {
    const ReplayData& R = *replay;
    const float goalT = R.time[R.goals[goal].first];
    goalReplay = GoalReplay{};
    goalReplay.on = true;
    goalReplay.goal = goal;
    goalReplay.from = std::max(goalT - GOAL_LEAD, Start());
    for (int cut : R.cuts)   // not back into the previous goal (a goal right after a kickoff)
        if (cut < R.goals[goal].first) goalReplay.from = std::max(goalReplay.from, R.time[cut + 1]);
    goalReplay.to = std::min(goalT + GOAL_TAIL, End());
    goalReplay.resume = resume;
    goalReplay.matchT = matchT;
    const int scorer = goal < static_cast<int>(R.scorers.size()) ? R.scorers[goal].first : -1;
    if (camMode != CAM_FREE && scorer >= 0) {
        goalReplay.camSet = true;
        goalReplay.camMode = camMode;
        goalReplay.camTarget = camTarget;
        goalReplay.ballCamManual = ballCamManual;
        camMode = CAM_PLAYER;
        camTarget = scorer;
        ballCamManual = scorer;   // his ball cam is ours meanwhile: on
        SetBallCamOf(CarOf(scorer), true);
    }
    rt = goalReplay.from;
    cvarManager->log("Replay Lab: replay du but " + std::to_string(goal + 1));
}

// The net an explosion borrowed (for its team's colours), freeplay's again.
void ReplayLab::GoalTeamBack(ServerWrapper sw) {
    if (!goalTeamBack.first || sw.IsNull()) return;
    ArrayWrapper<GoalWrapper> goals = sw.GetGoals();
    for (int i = 0; i < goals.Count(); i++)
        if (goals.Get(i).memory_address == goalTeamBack.first) goals.Get(i).SetTeamNum(goalTeamBack.second);
    goalTeamBack = {0, 0};
}

// Stops a goal's replay where it is (you simulate from there, look elsewhere): your camera back.
void ReplayLab::CancelGoalReplay() {
    if (!goalReplay.on) return;
    goalReplay.on = false;
    if (goalReplay.camSet) {
        camMode = goalReplay.camMode;
        camTarget = goalReplay.camTarget;
        ballCamManual = goalReplay.ballCamManual;
    }
}

void ReplayLab::EndGoalReplay() {
    if (!goalReplay.on) return;
    CancelGoalReplay();
    if (goalReplay.resume >= 0.f) {   // on to the kickoff
        rt = goalReplay.resume;
        return;
    }
    rt = goalReplay.to;   // a simulation's goal: the branch stays there
    playing = false;
    Status("Fin de la simulation. Croix bas (F11): recommencer   croix bas longue: sauvegarder   Back (F5): resimuler d'ici   "
           "croix haut longue: replay d'origine", 15.f);
}

// A simulation's goal shown again like the game's replay: its recording (a branch) is watched from a few seconds before
// the goal and stays there; F11 tries again, F5 simulates from any moment of it.
void ReplayLab::ReplaySimGoal() {
    goalReplayed = true;
    std::shared_ptr<ReplayData> branch = scenario;
    if (!branch || branch->goals.empty() || branch->Frames() < 2) return;
    replay = branch;
    mode = MODE_PLAYBACK;   // still while its cars are set up
    playing = false;
    const float goalT = branch->time[branch->goals.front().first];
    rt = std::max(goalT - GOAL_LEAD, Start());
    SetupCars([this, branch, goalT] {
        if (replay != branch) return;
        StartPlayback(rt, true);
        StartGoalReplay(0, -1.f, goalT);
    }, false);
}

/* ================================================================== goals and demolitions as they happen */

// Between two moments of the replay played forward: a demolished car blows up (it is back about 3 s later, when it is
// in the replay), and a goal blows the ball up. Not demolished again in a goal's replay: the car would not be back for
// the kickoff right after it.
void ReplayLab::ReplayEvents(ServerWrapper sw, float from, float to) {
    const ReplayData& R = *replay;
    auto between = [&](int frame) { return frame >= 0 && frame < R.Frames() && R.time[frame] > from && R.time[frame] <= to; };
    for (const auto& [frame, k] : R.demos) {
        if (goalReplay.on || !between(frame)) continue;
        CarWrapper car = CarOf(k);
        if (car.IsNull()) continue;
        car.Demolish();
        cvarManager->log("Replay Lab: " + R.names[k] + " demoli dans le replay");
    }
    for (size_t g = 0; g < R.goals.size(); g++) {
        if (!between(R.goals[g].first)) continue;
        const int scorer = g < R.scorers.size() ? R.scorers[g].first : -1;
        ExplodeBall(sw, R.goals[g].second, scorer, GoalSpot(static_cast<int>(g)));
    }
    if (R.phase.empty() || goalReplay.on) return;   // a goal's replay can start in its kickoff's countdown: not said again
    for (int f = std::max(FrameAt(from), 1); f <= FrameAt(to); f++) {   // the kickoff countdown: each number, then GO!
        if (!between(f)) continue;
        if (R.phase[f] == PHASE_COUNTDOWN && R.countdown[f] > 0 && R.countdown[f] != R.countdown[f - 1]) CountdownMessage(R.countdown[f]);
        if (R.phase[f] == PHASE_PLAYING && R.phase[f - 1] == PHASE_COUNTDOWN) CountdownMessage(0);
    }
}

// Where the ball went in: its last place at the goal's frame or before (a branch records no ball once it blew up).
Vector ReplayLab::GoalSpot(int goal) const {
    for (int f = replay->goals[goal].first; f >= 0; f--)
        if (replay->ball[f].valid) return StateAt(-1, replay->time[f]).pos;
    return Vector(0.f, 0.f, 0.f);
}

// The ball blows up where it went in, with the goal explosion of the player who scored: the game draws the scorer's own
// explosion, and his bot wears his. No scorer (an own goal): the stadium's.
void ReplayLab::ExplodeBall(ServerWrapper sw, int team, int scorer, Vector at) {
    BallWrapper ball = sw.GetBall();
    if (ball.IsNull() || !*opt.explosion) return;
    GoalWrapper goal(0);   // the goal it went in: the nearest
    float best = 1e9f;
    ArrayWrapper<GoalWrapper> goals = sw.GetGoals();
    for (int i = 0; i < goals.Count(); i++) {
        GoalWrapper g = goals.Get(i);
        if (g.IsNull()) continue;
        const float d = Len(Sub(g.GetLocation(), at));
        if (d < best) {
            best = d;
            goal = g;
        }
    }
    if (goal.IsNull()) return;
    // the goal is the scored-on team's (freeplay makes both team 1): the explosion takes the scoring team's colours;
    // freeplay's goal as it was at the next frame (OnFrame), once the explosion has its colours
    if (!goalTeamBack.first) goalTeamBack = {goal.memory_address, goal.GetTeamNum()};
    goal.SetTeamNum(static_cast<unsigned char>(1 - (team ^ (mirrored ? 1 : 0))));
    PriWrapper pri = scorer >= 0 ? PriOf(scorer) : PriWrapper(0);
    ball.SetLocation(at);
    ball.SetbAllowPlayerExplosionOverride(1);
    ball.Explode(goal, at, pri);
    ballGone = ballBlown = true;
    cvarManager->log("Replay Lab: le ballon explose (but " + std::string(team == 0 ? "bleu" : "orange") + ", explosion de " +
                     (pri.IsNull() ? std::string("personne") : pri.GetPlayerName().ToString()) + ")");
}

PriWrapper ReplayLab::PriOf(int k) const {
    if (k == CarPlayer()) {
        PlayerControllerWrapper pc = gameWrapper->GetPlayerController();
        return pc.IsNull() ? PriWrapper(0) : pc.GetPRI();
    }
    ServerWrapper sw = gameWrapper->GetGameEventAsServer();
    if (sw.IsNull()) return PriWrapper(0);
    ArrayWrapper<CarWrapper> cars = sw.GetCars();
    for (int i = 0; i < cars.Count(); i++) {
        CarWrapper car = cars.Get(i);
        if (!car.IsNull() && BotIndex(car) == k && !car.GetPRI().IsNull()) return car.GetPRI();
    }
    ArrayWrapper<PriWrapper> pris = sw.GetPRIs();   // his car is away (demolished): his player info
    for (int i = 0; i < pris.Count(); i++) {
        PriWrapper pri = pris.Get(i);
        if (!pri.IsNull() && pri.GetbBot() && pri.GetPlayerName().ToString() == BotName(k)) return pri;
    }
    return PriWrapper(0);
}

// After a goal the ball is gone until the kickoff, hidden and going through everything. A ball the game blew up does not
// play again: a new one comes (RenewBall), as the game makes one for the kickoff.
void ReplayLab::ShowBall(ServerWrapper sw, bool shown) {
    if (shown && !ballGone && !ballBlown) return;   // as it should be
    BallWrapper ball = sw.GetBall();
    if (!shown) {
        ballGone = true;
        if (ballBlown || ball.IsNull()) return;   // blown up: it dissolves by itself, going through everything
        if (!ball.GetbHidden()) ball.SetHidden2(1);
        if (ball.GetbCollideActors()) ball.SetbCollideActors(0);
        if (ball.GetbBlockActors()) ball.SetbBlockActors(0);
        return;
    }
    ballGone = false;
    if (ballBlown) {   // stays hidden until the new one comes
        ballBlown = false;
        ballRenew = true;
        return;
    }
    if (ball.IsNull()) return;
    if (ball.GetbHidden()) ball.SetHidden2(0);
    if (!ball.GetbCollideActors()) ball.SetbCollideActors(1);
    if (!ball.GetbBlockActors()) ball.SetbBlockActors(1);
}

// The new ball, made between two frames rather than in the middle of the physics step (a car's input), where the replay
// or the simulation has it.
void ReplayLab::RenewBall(ServerWrapper sw) {
    ballRenew = false;
    sw.ResetBalls();
    BallWrapper ball = sw.GetBall();
    if (ball.IsNull()) ball = sw.SpawnBall(Vector(0.f, 0.f, 93.f), true, false);
    cvarManager->log(std::string("Replay Lab: nouveau ballon") + (ball.IsNull() ? " introuvable" : ""));
    if (ball.IsNull()) return;
    if (ball.GetbHidden()) ball.SetHidden2(0);
    ball.SetbCollideActors(1);
    ball.SetbBlockActors(1);
    State s = StateAt(-1, mode == MODE_SIM ? forkT : Now());   // a simulation renews it as it starts
    if (!s.valid) return;
    if ((mode == MODE_PLAYBACK && !playing) || (mode == MODE_SIM && (waiting || kickoff > 0.f))) s.vel = s.ang = Vector(0, 0, 0);
    ball.SetLocation(s.pos);
    ball.SetRotation(s.rot);
    ball.SetVelocity(s.vel);
    ball.SetAngularVelocity(s.ang, false);
}

/* ================================================================== a simulation's goal */

// A moment after a simulation's goal (the cars played on), it is shown again; from the game's next frame, not from
// inside a car's input.
void ReplayLab::UpdateSimGoal() {
    if (goalTeam < 0 || goalReplayed || simT - goalT < SIM_AFTER_GOAL) return;
    goalReplayed = true;
    if (!*opt.revoirSimulation) return;
    gameWrapper->SetTimeout([this](GameWrapper*) {
        std::lock_guard<std::recursive_mutex> lock(mtx);
        if (mode == MODE_SIM && goalTeam >= 0) ReplaySimGoal();
    }, 0.01f);
}

void ReplayLab::OnHitBall(CarWrapper car) {
    std::lock_guard<std::recursive_mutex> lock(mtx);
    if (!replay || mode != MODE_SIM || car.IsNull()) return;
    CarWrapper local = gameWrapper->GetLocalCar();
    const int k = !local.IsNull() && Same(car, local) ? CarPlayer() : BotIndex(car);
    if (k >= 0 && k < replay->Players()) lastTouch[replay->teams[k]] = k;
    // a car that drives itself touched it: the ball is the game's from now on (a ghost's touch is the replay's)
    if (ballLocked && k >= 0 && k < replay->Players() && RoleOf(k) != ROLE_GHOST) {
        ballLocked = false;
        cvarManager->log("Replay Lab: le ballon ne suit plus le replay a +" + std::to_string(simT).substr(0, 5) + " s (touche par " +
                         replay->names[k] + ")");
    }
}

// The last player of the scoring team to touch the ball; none touched it since the simulation started (the ball followed
// the replay into the goal): whoever scored that goal in the replay.
int ReplayLab::SimGoalScorer(int team) const {
    if (lastTouch[team] >= 0) return lastTouch[team];
    const float t = Now();
    for (size_t g = 0; g < replay->goals.size() && g < replay->scorers.size(); g++)
        if (replay->goals[g].second == team && std::fabs(replay->time[replay->goals[g].first] - t) < 3.f) return replay->scorers[g].first;
    return -1;
}

/* ================================================================== on screen */

float ReplayLab::GoalKmh(int goal) const {
    const int f = replay->goals[goal].first;
    for (int i = f; i >= std::max(f - 3, 0); i--) {   // the last frame the ball still moved (it sleeps from the goal on)
        const RBody& b = replay->ball[i];
        if (b.valid && Len(b.vel) > 1.f) return Kmh(b.vel);
    }
    return 0.f;
}

std::string ReplayLab::PlayerText(int k) const {
    if (!replay || k < 0 || k >= replay->Players()) return "";
    const std::string text = Ascii(replay->names[k]);
    return text.find_first_not_of(' ') == std::string::npos ? BotName(k) : text;   // a name in another script: "Joueur 2"
}

// The game's own kickoff countdown, its numbers and sounds: 3, 2, 1, then GO! (0). Freeplay turns off the broadcast
// ones, the messages sent to a player still show (as kickoff training plugins do).
void ReplayLab::CountdownMessage(int n) {
    ServerWrapper sw = gameWrapper->GetGameEventAsServer();
    PlayerControllerWrapper pc = gameWrapper->GetPlayerController();
    if (sw.IsNull() || pc.IsNull() || !*opt.tableau) return;
    if (n > 0) sw.SendCountdownMessage(n, pc);
    else sw.SendGoMessage(pc);
}

// Like the game's: a band in the scoring team's colours, who scored, the assist and how fast the ball went in.
void ReplayLab::DrawGoal(CanvasWrapper& canvas, int team, int scorer, int assist, float kmh, float since) {
    if (since < 0.f || since > BANNER || team < 0 || team > 1) return;
    const float a = std::min(std::min(1.f, since / 0.15f), (BANNER - since) / 0.4f);
    const float y = canvas.GetSize().Y * 0.2f;
    Color(canvas, TEAM_RGB[team ^ (mirrored ? 1 : 0)], static_cast<int>(200 * a));
    canvas.SetPosition(Vector2F{0.f, y});
    canvas.FillBox(Vector2F{static_cast<float>(canvas.GetSize().X), 150.f});
    Color(canvas, 255, 255, 255, static_cast<int>(255 * a));
    Centered(canvas, "BUT !", 5.f, y + 8.f);
    Centered(canvas, scorer >= 0 ? PlayerText(scorer) : std::string(team == 0 ? "les bleus" : "les oranges"), 2.4f, y + 78.f);
    std::string more = assist >= 0 ? "passe de " + PlayerText(assist) : "";
    if (kmh > 1.f) more += (more.empty() ? "" : "   -   ") + std::to_string(static_cast<int>(kmh + 0.5f)) + " km/h";
    if (!more.empty()) Centered(canvas, more, 1.5f, y + 118.f);
}

// Like the game's replay of a goal: black bars above and below, REPLAY with a red dot bottom left, who scored at the
// bottom with the assist and the ball's speed.
void ReplayLab::DrawGoalReplay(CanvasWrapper& canvas) {
    const ReplayData& R = *replay;
    const int g = goalReplay.goal;
    if (g < 0 || g >= static_cast<int>(R.goals.size())) return;
    const int team = R.goals[g].second & 1;
    const float W = static_cast<float>(canvas.GetSize().X), H = static_cast<float>(canvas.GetSize().Y), bar = H * 0.08f;
    Color(canvas, 0, 0, 0, 255);
    canvas.SetPosition(Vector2F{0.f, 0.f});
    canvas.FillBox(Vector2F{W, bar});
    canvas.SetPosition(Vector2F{0.f, H - bar});
    canvas.FillBox(Vector2F{W, bar});
    const bool blink = std::fmod(rt - goalReplay.from, 1.f) < 0.6f;
    Color(canvas, 230, 30, 40, blink ? 255 : 90);
    canvas.SetPosition(Vector2F{40.f, H - bar + bar * 0.32f});
    canvas.FillBox(Vector2F{bar * 0.36f, bar * 0.36f});
    Color(canvas, 255, 255, 255, 255);
    canvas.SetPosition(Vector2F{40.f + bar * 0.6f, H - bar + bar * 0.22f});
    canvas.DrawString("REPLAY", 2.4f, 2.4f, true);

    const int scorer = g < static_cast<int>(R.scorers.size()) ? R.scorers[g].first : -1;
    const int assist = g < static_cast<int>(R.scorers.size()) ? R.scorers[g].second : -1;
    const float y = H - bar - 96.f;
    Color(canvas, TEAM_RGB[team ^ (mirrored ? 1 : 0)], 210);
    canvas.SetPosition(Vector2F{W * 0.3f, y});
    canvas.FillBox(Vector2F{W * 0.4f, 88.f});
    Color(canvas, 255, 255, 255, 255);
    Centered(canvas, scorer >= 0 ? "MARQUE PAR " + PlayerText(scorer) : std::string(team == 0 ? "BUT DES BLEUS" : "BUT DES ORANGES"), 2.2f, y + 8.f);
    std::string more = assist >= 0 ? "passe de " + PlayerText(assist) : "";
    const float kmh = GoalKmh(g);
    if (kmh > 1.f) more += (more.empty() ? "" : "   -   ") + std::to_string(static_cast<int>(kmh + 0.5f)) + " km/h";
    if (!more.empty()) Centered(canvas, more, 1.5f, y + 52.f);
}

// What the game shows of the match: who scored, a goal's replay, the end of the match (the kickoff countdown is the
// game's own: CountdownMessage);
// then the screen going dark around a jump in time. The match's own texts follow rlab_tableau, like the scoreboard.
void ReplayLab::DrawMatch(CanvasWrapper& canvas) {
    const ReplayData& R = *replay;
    const bool hud = *opt.tableau;
    if (mode == MODE_SIM) {
        if (!hud) return;
        if (goalTeam >= 0) DrawGoal(canvas, goalTeam, goalScorer, -1, goalKmh, simT - goalT);
        return;
    }
    const float t = rt;
    const int f = FrameAt(t);
    if (hud && goalReplay.on) {
        DrawGoalReplay(canvas);
    } else if (hud) {
        const int phase = PhaseAt(t);
        const int goal = GoalBefore(f);
        if (goal >= 0 && (phase == PHASE_GOAL || R.phase.empty())) {
            const int scorer = goal < static_cast<int>(R.scorers.size()) ? R.scorers[goal].first : -1;
            const int assist = goal < static_cast<int>(R.scorers.size()) ? R.scorers[goal].second : -1;
            DrawGoal(canvas, R.goals[goal].second, scorer, assist, GoalKmh(goal), t - R.time[R.goals[goal].first]);
        }
        if (!R.scenario && t >= End() - 0.05f) {   // like the game's end of the match
            int score[2];
            ScoreAt(End(), score);
            const std::string result = score[0] == score[1] ? "egalite" : score[0] > score[1] ? "victoire des bleus" : "victoire des oranges";
            const float y = canvas.GetSize().Y / 4.f;
            Color(canvas, 10, 12, 20, 200);
            canvas.SetPosition(Vector2F{0.f, y});
            canvas.FillBox(Vector2F{static_cast<float>(canvas.GetSize().X), 104.f});
            Color(canvas, 255, 255, 255, 255);
            Centered(canvas, "FIN DU MATCH", 4.f, y + 8.f);
            Color(canvas, 255, 220, 60, 255);
            Centered(canvas, result + "  " + std::to_string(score[0]) + " - " + std::to_string(score[1]), 1.6f, y + 70.f);
        }
    }
    const float dark = FadeAt(t);
    if (dark > 0.f) {
        Color(canvas, 0, 0, 0, static_cast<int>(255 * dark));
        canvas.SetPosition(Vector2{0, 0});
        canvas.FillBox(canvas.GetSize());
    }
}
