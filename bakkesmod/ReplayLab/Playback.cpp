// Every physics tick (your car's input, 120 per second of game time) everything that follows the replay is put where
// the replay has it: playback, the ghosts of a simulation, what each car does (boost, wheels).
#include "ReplayLab.h"
#include "LabMath.h"
#include "Rules.h"
#include "bakkesmod/wrappers/Engine/WorldInfoWrapper.h"
#include "bakkesmod/wrappers/GameObject/CarComponent/PrimitiveComponentWrapper.h"

using namespace lab;

// From a simulation (where removed players have no bot), the replay needs everybody back first.
void ReplayLab::PlayFrom(float t, bool play) {
    SetupCars([this, t, play] { StartPlayback(t, play); }, false);
}

void ReplayLab::StartPlayback(float t, bool play) {
    std::lock_guard<std::recursive_mutex> lock(mtx);
    if (!replay) return;
    CancelGoalReplay();   // a new moment: not a goal's replay any more (ReplaySimGoal starts its own after)
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

void ReplayLab::TogglePlay() {
    std::lock_guard<std::recursive_mutex> lock(mtx);
    if (goalReplay.on) {   // like the game: a goal's replay can be skipped
        rt = goalReplay.to;
        EndGoalReplay();
        return;
    }
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
    CancelGoalReplay();
    if (mode == MODE_SIM) PlayFrom(t, false);
    else rt = std::min(std::max(t, Start()), End());
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
    // what each car does, for the branch being recorded
    auto note = [&](int who) {
        if (who < 0 || who >= static_cast<int>(inputsNow.size())) return;
        auto byte = [](float v) { return static_cast<unsigned char>(std::lround(128.f + 127.f * std::min(std::max(v, -1.f), 1.f))); };
        inputsNow[who] = Sticks{byte(in->Throttle), byte(in->Steer), in->ActivateBoost || in->HoldingBoost, in->Handbrake != 0};
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
            if (kickoff > 0.f) {   // the kickoff countdown: nobody moves before GO
                ZeroInput(in);
                return;
            }
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
            if (CarPlayer() < 0) {   // you drive his bot: your controller goes to it, your car keeps still out of the way
                myInput = *in;
                ZeroInput(in);
            }
            return;
        }
        camInput = *in;   // your controller drives no car: it can fly the free camera, run the replay fast
        camInputAt = std::chrono::steady_clock::now();
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
        if (k >= 0 && mode == MODE_SIM && (waiting || kickoff > 0.f)) {   // bots and Nexto wait for you or GO too
            ZeroInput(in);
            return;
        }
        if (k >= 0 && mode == MODE_SIM && RoleOf(k) == ROLE_TOI) {   // the player's car you drive
            *in = myInput;
            note(k);
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
    // a car following the replay boosts, steers and powerslides like its player: its flames, its wheels (where it goes
    // stays the replay's: it is put there every tick)
    const bool follows = mode == MODE_PLAYBACK || RoleOf(k) == ROLE_GHOST;
    ZeroInput(in);
    if (follows) {
        const State s = StateAt(k, Now());
        in->ActivateBoost = in->HoldingBoost = s.boosting ? 1 : 0;
        if (mode == MODE_SIM || playing) {
            in->Throttle = s.throttle;
            in->Steer = s.steer;
            in->Handbrake = s.slide ? 1 : 0;
        }
    }
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
    if (ballRenew) RenewBall(sw);   // a blown-up ball's replacement, outside the physics step
    GoalTeamBack(sw);               // the net the explosion borrowed, freeplay's again
    UpdateCamera();
    AimCamera();   // also outside the car camera (after a demolition the game's camera watches the wreck)
}

void ReplayLab::Advance(float dt) {
    if (mode == MODE_SIM) {
        if (kickoff > 0.f) {   // the kickoff countdown: everything still until GO
            const float before = kickoff;
            kickoff -= dt;
            if (kickoff <= 0.f) {
                kickoff = 0.f;
                CountdownMessage(0);
                PlaceAtFork(true);
            } else if (std::ceil(kickoff) < std::ceil(before)) {
                CountdownMessage(static_cast<int>(std::ceil(kickoff)));
            }
        } else if (!waiting) {
            simT += dt;   // game time: it already runs at the simulation speed (rlab_speed)
        }
        return;
    }
    // while you watch, your controller's throttle runs the replay fast, forward or back, over the holes in time (the free
    // camera flies with it instead)
    const bool pad = CarPlayer() < 0 && ShownMode() != CAM_FREE && !goalReplay.on && *opt.manette;
    const float drive = pad ? Pad().Throttle : 0.f;
    if (std::fabs(drive) > 0.3f) {
        const float t = std::min(std::max(rt + dt * FAST * (drive > 0.f ? 1.f : -1.f), Start()), End());
        const int i = FrameAt(t);
        rt = HoleAfter(i) && t > replay->time[i] ? replay->time[drive > 0.f ? i + 1 : i] : t;
        return;
    }
    if (playing) AdvancePlayback(dt);
}

// Moves everything that follows the replay to where it is at this moment.
void ReplayLab::Apply() {
    ServerWrapper sw = gameWrapper->GetGameEventAsServer();
    if (sw.IsNull()) return;
    if (mode == MODE_SIM && (waiting || kickoff > 0.f)) {   // nothing moves before you do, or before GO
        PlaceAtFork(false);
        return;
    }
    const float t = Now();
    applyTicks++;
    const float before = lastApplyT;
    const bool flowing = t > before && t - before < 0.2f;   // the replay went on, no jump on the timeline
    lastApplyT = t;
    if (lastValid.size() != replay->names.size()) lastValid.assign(replay->names.size(), 0);
    // the replay's goals and demolitions in between, before the cars move (a demolished car blows up where it was)
    if (mode == MODE_PLAYBACK && flowing) ReplayEvents(sw, before, t);
    // where things are in the replay; while the replay is paused they keep still instead of moving one tick each time
    const bool still = mode == MODE_PLAYBACK && !playing;
    auto at = [&](int k) {
        State s = StateAt(k, t);
        if (still) s.vel = s.ang = Vector(0, 0, 0);
        return s;
    };

    // rlab_debug 1: twice a second, where each car should be (replay) vs where the game has it
    const bool debug = (*opt.debug != 0) && (++debugFrame % 70 == 0);
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
    const bool localFollows = CarPlayer() >= 0 && mode == MODE_PLAYBACK;
    if (!local.IsNull()) CarRules(local, CarPlayer() >= 0 || Driving());
    if (!local.IsNull() && Driving()) {   // the game's boost meter shows your car's: it gets the boost of the car you drive
        CarWrapper his = CarOf(ToiIndex());
        BoostWrapper mine = local.GetBoostComponent();
        BoostWrapper theirs = his.IsNull() ? BoostWrapper(0) : his.GetBoostComponent();
        if (!mine.IsNull() && !theirs.IsNull()) mine.SetBoostAmount(theirs.GetCurrentBoostAmount());
    }
    if (!local.IsNull()) Stash(local, CarPlayer() < 0);
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
        if (*opt.debug != 2) burst = 0;
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
        else if (wasThere && flowing && !replay->demosKnown && !goalReplay.on) local.Demolish();   // a branch: gone, so demolished
        else Freeze(local);
    } else if (debug) {
        cvarManager->log(local.IsNull() ? "dbg ta voiture: absente (demolie ?)"
                                        : CarPlayer() < 0 ? "dbg ta voiture: attend hors du terrain" : "dbg ta voiture: tu conduis");
    }
    ArrayWrapper<CarWrapper> cars = sw.GetCars();
    for (int i = 0; i < cars.Count(); i++) {   // a bot the game created again (after a reset) gets its player back
        CarWrapper car = cars.Get(i);
        if (BotIndex(car) == -1 && OurBot(car)) {
            MapBots(mode == MODE_SIM);
            break;
        }
    }
    // the replay scores here but the simulation does not (the ball went elsewhere): what the ghosts do next (thrown by
    // its explosion, then its kickoff) is not this game's, the Psyonix AI plays them
    if (mode == MODE_SIM && !replayScored && !ballLocked && goalTeam < 0)
        for (const auto& goal : replay->goals) {
            const float at = replay->time[goal.first];
            if (at <= forkT || at > t) continue;
            replayScored = true;
            cvarManager->log("Replay Lab: le replay marque ici, pas la simulation: les fantomes jouent en bot Psyonix");
            break;
        }
    // the cars that drive themselves in a simulation (yours, bots): a ghost they come against stops following the replay
    freeCars.clear();
    if (mode == MODE_SIM) {
        if (!local.IsNull() && CarPlayer() >= 0) freeCars.push_back(local.GetLocation());
        for (int i = 0; i < cars.Count(); i++) {
            CarWrapper car = cars.Get(i);
            const int k = car.IsNull() || car.GetbHidden() || (!local.IsNull() && Same(car, local)) ? -1 : BotIndex(car);
            if (k >= 0 && RoleOf(k) != ROLE_GHOST) freeCars.push_back(car.GetLocation());
        }
    }
    // a demolished car leaves the game (a new one comes back 3 seconds later): in a simulation, its player was demolished
    onPitch.assign(replay->names.size(), 0);
    const unsigned long hidePlate = !*opt.noms || *opt.nomsPlugin ? 1 : 0;   // the game's, unless ours or none
    for (int i = 0; i < cars.Count(); i++) {
        CarWrapper car = cars.Get(i);
        if (car.IsNull() || (!local.IsNull() && Same(car, local))) continue;
        const int k = BotIndex(car);
        if (debug && k == -1) cvarManager->log("dbg voiture inconnue: " + NameOf(car));
        if (k == PARKED) Park(car);
        if (k < 0 || car.GetbHidden()) continue;
        onPitch[k] = true;
        CarRules(car, DrivesItself(k));
        // the game's name plate, or none when ours (rlab_noms_plugin, in the team's colour) stands in for it
        PrimitiveComponentWrapper plate = car.GetNameplateMeshComponent();
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
        if (mode == MODE_SIM && Bumped(car, k)) continue;            // touched: the game's physics, then the Psyonix AI
        if (s.valid && mode == MODE_SIM) TrackCar(car, s);
        else if (s.valid) ApplyCar(car, s);
        else if (wasThere && flowing && !replay->demosKnown && !goalReplay.on) {   // a branch: gone at that moment, so demolished
            car.Demolish();
            cvarManager->log("Replay Lab: " + replay->names[k] + " demoli dans la branche");
        }
        else Park(car);   // not on the pitch at that moment: demolished (a replay blew it up above), removed in a branch
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

    if (mode == MODE_SIM) {   // recorded as a branch (with or without a ball: a goal blows it up)
        UpdatePads();
        UpdateSimGoal();
        RecordFrame(sw);
    }
    // after a goal the ball is gone until the kickoff (blown up), as in the game
    if (mode == MODE_PLAYBACK) ShowBall(sw, !*opt.explosion || !BallGoneAt(t));
    if (mode == MODE_PLAYBACK) SyncPads(t);
    BallWrapper ball = sw.GetBall();
    if (ball.IsNull()) return;
    const State real = at(-1);
    bool follow = mode == MODE_PLAYBACK;
    if (mode == MODE_SIM && ballLocked) {
        // It follows the replay until a car that drives itself touches it (OnHitBall), or until the replay has it touched
        // by a player who no longer follows it (that touch will not come): from then on it is the game's.
        bool release = !real.valid;
        for (int k = 0; k < replay->Players() && !release; k++) {
            if (RoleOf(k) == ROLE_GHOST) continue;
            const State r = StateAt(k, t + TICK);                         // where the real car is about to be
            if (r.valid && Len(Sub(r.pos, real.pos)) < TOUCH_DIST) release = true;
        }
        if (release) {
            ballLocked = false;
            cvarManager->log("Replay Lab: le ballon ne suit plus le replay a +" + std::to_string(simT).substr(0, 5) + " s" +
                             (real.valid ? " (un joueur change le touchait dans le replay)" : " (le replay ne l'a plus)"));
        } else {
            follow = true;
        }
    }
    if (mode == MODE_SIM) {
        UpdateMatch(ball);
        if (ballGone) ShowBall(sw, false);   // blown up by the simulation's goal: away until the next simulation
    }
    if (ballGone) follow = false;            // what is left of a blown-up ball is not moved
    if (mode == MODE_SIM && match == MATCH_OVER && !ballGone) {   // the ball stays where the match ended
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
}

void ReplayLab::ApplyCar(CarWrapper car, const State& s) {
    car.SetLocation(s.pos);
    car.SetRotation(s.rot);
    car.SetVelocity(s.vel);
    car.SetAngularVelocity(s.ang, false);
    BoostWrapper boost = car.GetBoostComponent();
    if (!boost.IsNull()) boost.SetBoostAmount(s.boost);
}

// In a simulation a ghost is driven along its real path by its speed rather than put on it: it collides like a car
// (a bump moves it, it pushes back) and comes back to the path; knocked too far from it, it is put back.
// A ghost is pulled back onto the replay's path every tick, so against it a car hits a wall. When a car that drives
// itself comes against it (a kickoff, a 50/50) it is let go just before they touch: the collision is the game's, and the
// Psyonix AI plays him from then on, as after a demolition.
bool ReplayLab::Bumped(CarWrapper car, int k) {
    if (k >= static_cast<int>(bumped.size())) bumped.resize(replay->names.size(), false);
    if (bumped[k]) return true;
    const Vector at = car.GetLocation();
    for (const Vector& other : freeCars)
        if (Len(Sub(other, at)) < BUMP_DIST) {
            bumped[k] = true;
            cvarManager->log("Replay Lab: " + replay->names[k] + " touche: il ne suit plus le replay (bot Psyonix)");
            return true;
        }
    return false;
}

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
    const size_t P = static_cast<size_t>(R.Players());
    auto body = [&](int f) -> const RBody& { return k < 0 ? R.ball[f] : R.cars[f * P + k]; };
    int i = FrameAt(t);
    const int j = std::min(i + 1, n - 1);
    // not there yet but an instant later (a kickoff's cars and ball come a frame after it starts): as it will be
    if (!body(i).valid && body(j).valid && R.time[j] - t < 0.1f) i = j;
    const RBody& a = body(i);
    const RBody& b0 = body(j);
    if (!a.valid) return s;
    const RBody& b = b0.valid ? b0 : a;
    const float span = HoleAfter(i) ? 0.f : R.time[j] - R.time[i];   // across a hole in time it holds until the jump
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
        const size_t at = i * P + k;
        s.boost = R.boost[at] / 255.f;
        s.boosting = R.boosting[at] != 0;
        if (!R.throttle.empty()) {
            s.throttle = std::max(-1.f, (R.throttle[at] - 128) / 127.f);
            s.steer = std::max(-1.f, (R.steer[at] - 128) / 127.f);
        }
        s.slide = !R.moves.empty() && (R.moves[at] & 8) != 0;
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
