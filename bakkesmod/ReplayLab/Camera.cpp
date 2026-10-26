// Cameras like in the game's replay viewer, for the replay, the simulation and a scenario watched again.
// BakkesMod does not let freeplay change camera mode, and the game works out its camera after anything a plugin can
// reach, so every camera here is the game's own camera made to follow a car. Like the replay viewer, it then takes
// that car's player's camera settings and ball cam (from his camera settings actor):
//   game       your car
//   player     any player's car, with his own camera settings, ball cam on and off when he switched it
//              (F4: next player, F8: ball cam yourself)
//   automatic  a director: the player nearest the ball, with ball cam
//   free       an invisible car flown with your controller, the camera behind it (F8: looking at the ball)
// F7 goes to the next one.
#include "ReplayLab.h"
#include "LabMath.h"
#include "bakkesmod/wrappers/GameObject/CameraSettingsActorWrapper.h"

#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

using namespace lab;

namespace {
constexpr float DEG = 182.044f;          // Unreal rotator units per degree
constexpr float SHOT = 2.5f;             // the director keeps a shot at least this long (seconds)
const char* CAM_LABELS[CAM_COUNT] = {"du jeu", "joueur", "libre", "automatique"};
const char* DOLLY = "LabCam";            // the invisible car the free camera follows
constexpr int OCTANE_BODY = 23;
constexpr size_t ACTOR_BYTES = 0x340;    // the part of a camera settings actor searched for its flags

CameraSettingsActorWrapper SettingsActor(CarWrapper car) {
    if (car.IsNull() || car.GetPRI().IsNull()) return CameraSettingsActorWrapper(0);
    return car.GetPRI().GetPersistentCamera();
}

bool ReadActor(uintptr_t address, std::vector<unsigned char>& out) {
    out.assign(ACTOR_BYTES, 0);
    SIZE_T got = 0;
    return ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<void*>(address), out.data(), out.size(), &got) && got == out.size();
}
}  // namespace

CarWrapper ReplayLab::CarOf(int k) const {
    if (k == CarPlayer()) return gameWrapper->GetLocalCar();
    ServerWrapper sw = gameWrapper->GetGameEventAsServer();
    if (sw.IsNull()) return CarWrapper(0);
    ArrayWrapper<CarWrapper> cars = sw.GetCars();
    for (int i = 0; i < cars.Count(); i++) {
        CarWrapper car = cars.Get(i);
        if (!car.IsNull() && !car.GetbHidden() && BotIndex(car) == k && car.GetLocation().Z > -100.f) return car;
    }
    return CarWrapper(0);
}

CarWrapper ReplayLab::Dolly() const {
    ServerWrapper sw = gameWrapper->GetGameEventAsServer();
    if (sw.IsNull()) return CarWrapper(0);
    ArrayWrapper<CarWrapper> cars = sw.GetCars();
    for (int i = 0; i < cars.Count(); i++)
        if (!cars.Get(i).IsNull() && NameOf(cars.Get(i)) == DOLLY) return cars.Get(i);
    return CarWrapper(0);
}

int ReplayLab::ShownMode() const {
    // the invisible car could touch the ball of a simulation: the free camera is for replays and scenarios
    if (camMode == CAM_FREE && mode == MODE_SIM) return CAM_AUTO;
    // while you watch your car waits under the field: the camera is on a player
    if (camMode == CAM_GAME && CarPlayer() < 0) return CAM_PLAYER;
    return camMode;
}

std::string ReplayLab::CamText() const {
    std::string text = std::string("camera ") + CAM_LABELS[ShownMode()];
    if ((ShownMode() == CAM_PLAYER || ShownMode() == CAM_AUTO) && replay && camTarget < replay->Players())
        text += " " + replay->names[camTarget];
    if (camMode == CAM_FREE && mode == MODE_SIM) text += " (la libre: en revoyant)";
    return text;
}

void ReplayLab::NextCamMode() {
    std::lock_guard<std::recursive_mutex> lock(mtx);
    camMode = (camMode + 1) % CAM_COUNT;
    ballCamManual = -1;
    if (replay && CarPlayer() >= 0) camTarget = CarPlayer();
    camShotStart = std::chrono::steady_clock::now();
    if (camMode == CAM_FREE) {   // starts where the camera is
        CameraWrapper cam = gameWrapper->GetCamera();
        if (!cam.IsNull()) {
            freePos = cam.GetLocation();
            freeRot = Rotator{0, cam.GetRotation().Yaw, 0};
        }
    }
    static const char* HELP[CAM_COUNT] = {
        "", "  F4 joueur suivant, F8 camera ballon",
        "  gachettes: avancer/reculer, stick: tourner et monter/descendre, saut/powerslide: monter/descendre, boost: vite, F8: viser le ballon",
        "  suit le joueur le plus proche du ballon"};
    Status("Camera " + std::string(CAM_LABELS[camMode]) + HELP[camMode], 8.f);
}

void ReplayLab::NextCamTarget() {
    std::lock_guard<std::recursive_mutex> lock(mtx);
    if (!replay) return;
    const int n = replay->Players();
    ballCamManual = -1;
    for (int step = 1; step <= n; step++) {
        const int k = (camTarget + step) % n;
        if (!CarOf(k).IsNull()) {
            camTarget = k;
            break;
        }
    }
    Status(CamText(), 4.f);
}

CarWrapper ReplayLab::Followed() const {
    CameraWrapper cam = gameWrapper->GetCamera();
    if (cam.IsNull() || !cam.GetViewTarget().Target) return gameWrapper->GetLocalCar();
    return CarWrapper(reinterpret_cast<uintptr_t>(cam.GetViewTarget().Target));
}

// The SDK cannot set the ball cam flag of a camera settings actor, only the rear view flag declared right after it in the
// game's code: switching that one and back shows where they are (found once, at the first use).
bool ReplayLab::FindBallCamFlag(CarWrapper car) {
    if (ballCamByte != -1) return ballCamByte >= 0;
    CameraSettingsActorWrapper csa = SettingsActor(car);
    if (csa.IsNull()) return false;
    std::vector<unsigned char> before, after;
    const bool behind = csa.GetUsingBehindView();
    const bool read = ReadActor(csa.memory_address, before);
    csa.SetUsingBehindView(!behind);
    const bool read2 = ReadActor(csa.memory_address, after);
    csa.SetUsingBehindView(behind);
    int changed = 0, at = -1, bit = -1;
    for (size_t i = 0; read && read2 && i < ACTOR_BYTES; i++) {
        const unsigned char d = before[i] ^ after[i];
        if (!d) continue;
        changed++;
        at = static_cast<int>(i);
        for (bit = 0; bit < 8 && !(d & (1 << bit)); bit++) {}
        if (d != (1 << bit)) changed++;   // more than one bit: not a flag
    }
    if (changed != 1) {
        ballCamByte = -2;
        cvarManager->log("Replay Lab: drapeau de camera ballon introuvable (" + std::to_string(changed) + " changements)");
        return false;
    }
    ballCamByte = bit > 0 ? at : at - 1;
    ballCamBit = bit > 0 ? bit - 1 : 7;
    cvarManager->log("Replay Lab: camera ballon des joueurs a l'octet " + std::to_string(ballCamByte) + ", bit " + std::to_string(ballCamBit));
    return true;
}

bool ReplayLab::BallCamOf(CarWrapper car) const {
    CameraSettingsActorWrapper csa = SettingsActor(car);
    if (csa.IsNull() || ballCamByte < 0) return false;
    return (*reinterpret_cast<const unsigned char*>(csa.memory_address + ballCamByte) >> ballCamBit) & 1;
}

void ReplayLab::SetBallCamOf(CarWrapper car, bool on) {
    if (!FindBallCamFlag(car) || BallCamOf(car) == on) return;
    CameraSettingsActorWrapper csa = SettingsActor(car);
    if (csa.IsNull()) return;
    *reinterpret_cast<unsigned char*>(csa.memory_address + ballCamByte) ^= static_cast<unsigned char>(1 << ballCamBit);
    ballCamWrites++;
}

bool ReplayLab::BallCamOn() const {
    CarWrapper car = Followed();
    if (ballCamByte >= 0 && !SettingsActor(car).IsNull()) return BallCamOf(car);
    CameraWrapper cam = gameWrapper->GetCamera();
    return !cam.IsNull() && cam.GetCameraState().find("LookAtBall") != std::string::npos;   // CameraState_LookAtBall_TA
}

// your car: as if you pressed your ball cam button; another one: its player's ball cam
void ReplayLab::ToggleBallCam(bool byYou) {
    CarWrapper car = Followed();
    CarWrapper local = gameWrapper->GetLocalCar();
    if (car.IsNull() || car.memory_address == local.memory_address || !FindBallCamFlag(car)) {
        PlayerControllerWrapper pc = gameWrapper->GetPlayerController();
        if (pc.IsNull()) return;
        pc.PressSecondaryCamera();
        pc.ReleaseSecondaryCamera();
    } else {
        SetBallCamOf(car, !BallCamOf(car));
    }
    if (!byYou) return;
    if (ShownMode() == CAM_PLAYER) ballCamManual = camTarget;
    gameWrapper->SetTimeout([this](GameWrapper*) { Status(std::string("Camera ballon ") + (BallCamOn() ? "activee" : "desactivee"), 3.f); },
                            0.2f);
}

// The game's camera takes the settings of the player whose car it follows (in a bot's: Psyonix's defaults), so a
// player's view has his own settings when his bot has them, as in the replay viewer; yours when the replay has none.
void ReplayLab::CameraSettings(ServerWrapper sw) {
    CarWrapper local = gameWrapper->GetLocalCar();
    if (local.IsNull() || local.GetPRI().IsNull()) return;
    const ProfileCameraSettings yours = local.GetPRI().GetCameraSettings();
    const bool his = cvarManager->getCvar("rlab_cam_joueur").getBoolValue();
    auto same = [](const ProfileCameraSettings& a, const ProfileCameraSettings& b) {
        return a.FOV == b.FOV && a.Height == b.Height && a.Pitch == b.Pitch && a.Distance == b.Distance && a.Stiffness == b.Stiffness &&
               a.SwivelSpeed == b.SwivelSpeed && a.TransitionSpeed == b.TransitionSpeed;
    };
    ArrayWrapper<CarWrapper> cars = sw.GetCars();
    for (int i = 0; i < cars.Count(); i++) {
        CarWrapper car = cars.Get(i);
        if (car.IsNull() || car.memory_address == local.memory_address || car.GetPRI().IsNull()) continue;
        const int k = BotIndex(car);
        const ProfileCameraSettings want =
            his && k >= 0 && k < static_cast<int>(replay->cams.size()) && replay->cams[k].known ? replay->cams[k].settings : yours;
        if (!same(car.GetPRI().GetCameraSettings(), want)) car.GetPRI().SetCameraSettings(want);
    }
}

// Every rendered frame: the director's choice, and where the invisible car of the free camera goes.
void ReplayLab::UpdateCamera() {
    const auto now = std::chrono::steady_clock::now();
    const float dt = std::min(std::chrono::duration<float>(now - camLast).count(), 0.1f);
    camLast = now;
    ServerWrapper sw = gameWrapper->GetGameEventAsServer();
    if (sw.IsNull() || !replay || mode == MODE_IDLE) return;
    if (std::chrono::duration<float>(now - camSettingsCheck).count() > 0.5f) {
        camSettingsCheck = now;
        CameraSettings(sw);
    }
    CarWrapper dolly = Dolly();
    if (ShownMode() == CAM_AUTO) {
        BallWrapper ball = sw.GetBall();
        if (!ball.IsNull()) {
            auto distance = [&](int k) {
                CarWrapper car = CarOf(k);
                return car.IsNull() ? 1e9f : Len(Sub(car.GetLocation(), ball.GetLocation()));
            };
            int best = camTarget;
            for (int k = 0; k < replay->Players(); k++)
                if (distance(k) < distance(best)) best = k;
            const bool held = std::chrono::duration<float>(now - camShotStart).count() < SHOT;
            if (best != camTarget && (distance(camTarget) > 1e8f || (!held && distance(best) < 0.7f * distance(camTarget)))) {
                camTarget = best;
                camShotStart = now;
            }
        }
        // ball cam on (your car's: now and then checked, the button takes effect over a few frames)
        CarWrapper car = Followed();
        if (!car.IsNull() && car.memory_address != gameWrapper->GetLocalCar().memory_address) {
            SetBallCamOf(car, true);
        } else if (std::chrono::duration<float>(now - camBallCheck).count() > 1.f) {
            camBallCheck = now;
            if (!BallCamOn()) ToggleBallCam(false);
        }
    }
    // like the replay viewer: a player's view switches ball cam when he switched it
    const bool his = ShownMode() == CAM_PLAYER && camTarget != CarPlayer() && cvarManager->getCvar("rlab_cam_joueur").getBoolValue() &&
                     camTarget < static_cast<int>(replay->cams.size()) && replay->cams[camTarget].known;
    if (his && ballCamManual != camTarget) SetBallCamOf(CarOf(camTarget), replay->cams[camTarget].BallCamAt(Now()));
    if (ShownMode() != CAM_FREE) {
        if (!dolly.IsNull() && dolly.GetLocation().Z > -500.f) Park(dolly);
        return;
    }
    if (dolly.IsNull()) {   // made once, when the free camera is first used
        if (!dollyAsked) sw.SpawnBot(OCTANE_BODY, DOLLY);
        dollyAsked = true;
        return;
    }
    // invisible and going through everything
    if (!dolly.GetbHidden()) dolly.SetHidden2(1);
    if (dolly.GetbCollideActors()) dolly.SetbCollideActors(0);
    if (dolly.GetbBlockActors()) dolly.SetbBlockActors(0);
    // flown with your controller (read while it drives no car)
    const ControllerInput& in = camInput;
    freeRot.Yaw += static_cast<int>(in.Steer * 100.f * DEG * dt);
    const Axes ax = AxesOf(freeRot);
    const float speed = (in.ActivateBoost || in.HoldingBoost) ? 3200.f : 1100.f;
    Vector move = Add(Mul(ax.f, in.Throttle), Mul(ax.r, in.Roll));
    move.Z += (in.Jump ? 1.f : 0.f) - (in.Handbrake ? 1.f : 0.f) - in.Pitch;
    freePos = Add(freePos, Mul(move, speed * dt));
    freePos.Z = std::min(std::max(freePos.Z, 20.f), 2000.f);
    // the game's camera sits behind its car: the car goes that far in front of where the camera should be
    const ProfileCameraSettings settings = gameWrapper->GetCamera().IsNull() ? ProfileCameraSettings{} : gameWrapper->GetCamera().GetCameraSettings();
    dolly.SetLocation(Add(Add(freePos, Mul(ax.f, settings.Distance)), Vector(0, 0, -settings.Height)));
    dolly.SetRotation(freeRot);
    dolly.SetVelocity(Vector(0, 0, 0));
    dolly.SetAngularVelocity(Vector(0, 0, 0), false);
}

// Just before the game works out its camera: the car it follows.
void ReplayLab::AimCamera() {
    if (!replay || mode == MODE_IDLE || !gameWrapper->IsInFreeplay()) return;
    CameraWrapper cam = gameWrapper->GetCamera();
    if (cam.IsNull()) return;
    CarWrapper target = gameWrapper->GetLocalCar();
    if (ShownMode() == CAM_PLAYER || ShownMode() == CAM_AUTO) target = CarOf(camTarget);
    if (ShownMode() == CAM_FREE) target = Dolly();
    for (int k = 0; target.IsNull() && replay && k < replay->Players(); k++) target = CarOf(k);   // his car is gone: another
    if (target.IsNull()) target = gameWrapper->GetLocalCar();
    if (target.IsNull()) return;
    ViewTarget vt = cam.GetViewTarget();
    if (vt.Target == reinterpret_cast<void*>(target.memory_address)) return;
    vt.Target = reinterpret_cast<void*>(target.memory_address);
    cam.SetViewTarget(vt);
}
