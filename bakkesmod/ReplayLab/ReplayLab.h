#pragma once
// ReplayLab: watch any replay inside freeplay and take over at any moment, with real game physics.
// Standalone: replays are read straight from the Demos folder by rlab_replay.dll (Rust, subtr-actor).
//   F3   open/close the Replay Lab window (replay list, timeline, roles, saves, options)
//   F4   watch from the next player's car (your car takes that player's place)
//   F9   play / pause the replay (during a simulation: back to the replay)
//   F10  simulate from here (in the game's own replay viewer: open this replay at this moment and simulate)
//   F11  restart the simulation from the same moment
// Console: rlab_open <replay id> [frame] [1 = simulate right away]

#include "bakkesmod/plugin/bakkesmodplugin.h"
#include "bakkesmod/plugin/PluginSettingsWindow.h"
#include "bakkesmod/plugin/pluginwindow.h"
#include "bakkesmod/wrappers/includes.h"
#include "nexto.h"

#include <array>
#include <chrono>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

struct RBody {
    Vector pos{0, 0, 0};
    float quat[4] = {0, 0, 0, 1};   // x, y, z, w as stored in replays
    Vector vel{0, 0, 0};
    Vector ang{0, 0, 0};            // rad/s
    bool valid = false;
};

// state used to drive an actor in the game
struct State {
    Vector pos{0, 0, 0};
    Rotator rot{0, 0, 0};
    Vector vel{0, 0, 0};
    Vector ang{0, 0, 0};
    float boost = 0.f;              // 0..1
    bool boosting = false;
    bool valid = false;
};

// a player's car as it looks in the replay
struct Look {
    bool known = false;
    std::vector<std::array<int, 3>> slots;   // loadout slot, product, paint (-1: none)
    int teamColor = 0, accentColor = 0, teamFinish = 0, accentFinish = 0;
    int Body() const;                        // the product in slot 0 (an Octane when unknown)
};

// a player's camera as the replay recorded it
struct PlayerCam {
    bool known = false;
    ProfileCameraSettings settings{};
    std::vector<std::pair<float, bool>> ballCam;   // (replay time, on) each time the player switched ball cam
    bool BallCamAt(float t) const;
};

// the game modes a freeplay can play like the replay (its ball, its goals)
enum Kind { KIND_SOCCAR, KIND_HOOPS, KIND_SNOWDAY, KIND_DROPSHOT, KIND_COUNT };

struct ReplayData {
    std::string id, map, date;
    std::string ballArch;                       // the ball the replay's game used ("Archetypes.Ball.Ball_BasketBall"...)
    std::vector<std::string> names;
    std::vector<int> teams;
    std::vector<float> time;
    std::vector<int> secs;
    std::vector<RBody> ball;                    // [frame]
    std::vector<RBody> cars;                    // [frame * players + k]
    std::vector<unsigned char> boost, boosting; // same indexing as cars
    std::vector<unsigned char> moves;           // same indexing: 1 jumping, 2 double jumping, 4 dodging
    int baseScore[2] = {0, 0};                  // goals before the first frame (a scenario starts from a score)
    std::vector<std::pair<int, int>> goals;     // (frame, scoring team)
    std::vector<std::pair<int, int>> saves;     // (frame, player)
    std::vector<Look> looks;                    // [player]
    std::vector<PlayerCam> cams;                // [player]
    bool scenario = false;                      // a simulation recorded in the game, not a replay file
    int otFrame = -1;                           // overtime starts after this frame
    int Players() const { return static_cast<int>(names.size()); }
    int Frames() const { return static_cast<int>(time.size()); }
};

// what a car still has in the air: its jump, its flip (a dodge or a double jump)
struct Air {
    bool jumped = false;     // jumped since it left the floor, a wall or the ceiling
    bool flipped = false;    // its flip used (and not given back by the ball: a flip reset)
    float sinceJump = 0.f;   // seconds since the jump ended (the flip is there for 1.25 s after it)
};

// the match in a simulation: the clock runs from the replay's, and at 0:00 the match ends when the ball touches the ground
enum Match { MATCH_PLAYING, MATCH_ZERO, MATCH_OVERTIME, MATCH_OVER };

struct ReplayEntry {
    std::string id, path, map, date, label;
};

enum Role { ROLE_TOI, ROLE_GHOST, ROLE_REMOVED, ROLE_BOT, ROLE_FREE, ROLE_NEXTO, ROLE_COUNT };
enum Mode { MODE_IDLE, MODE_PLAYBACK, MODE_SIM };
enum CamMode { CAM_GAME, CAM_PLAYER, CAM_FREE, CAM_AUTO, CAM_COUNT };

class ReplayLab : public BakkesMod::Plugin::BakkesModPlugin,
                  public BakkesMod::Plugin::PluginSettingsWindow,
                  public BakkesMod::Plugin::PluginWindow {
public:
    void onLoad() override;
    void onUnload() override;

    // F2 > Plugins tab
    void RenderSettings() override;
    std::string GetPluginName() override { return "Replay Lab"; }
    void SetImGuiContext(uintptr_t ctx) override;

    // separate window, toggled with F3 ("togglemenu replaylab")
    void Render() override;
    std::string GetMenuName() override { return "replaylab"; }
    std::string GetMenuTitle() override { return "Replay Lab"; }
    bool ShouldBlockInput() override;
    bool IsActiveOverlay() override { return true; }
    void OnOpen() override { windowOpen = true; }
    void OnClose() override { windowOpen = false; }

private:
    // loading
    bool LoadReader();
    std::wstring DemosFolder() const;
    void RefreshList();
    void OpenReplay(const std::string& id, int startFrame, bool simulate, const std::string& followed);
    void OpenFromViewer();
    bool Decode(const unsigned char* data, size_t len, ReplayData& out) const;

    // freeplay scene
    void EnterFreeplay(std::function<void()> then);
    bool InReplayStadium() const;              // the replay's map, in the replay's mode
    int GameKind() const;                      // the replay's mode
    void WaitForFreeplay(int tries, std::function<void()> then);
    bool NeedsBot(int k, bool forSim) const;
    void SetupCars(std::function<void()> then, bool forSim);
    void SpawnBots(std::vector<int> missing, int tries, std::function<void()> then, bool forSim);
    void MapBots(bool forSim);
    void DressCars();                          // each car: the team of the player it stands for
    void OnSpawnCar();                         // a bot of ours being made: the look and team of its player, before its car
    void Dress(PriWrapper pri, int k);         // a bot's player info: the look and team of player k
    void Redress(CarWrapper car, int k);       // ... then its car blown up, to come back with them
    int GameTeam(int k) const;                 // team in the game (the replay's, swapped if the scene is turned around)
    void PlayFrom(float t, bool play);
    void StartPlayback(float t, bool play);
    void Fork();
    void PlaceAtFork(bool moving);               // everything as at the moment the simulation starts from
    void BackToReplay();
    void TogglePlay();
    void Seek(float t);
    void OnSimulateKey();
    void NextView();
    void NextRole();                           // controller: the next role for the player in view (applied after a pause)
    void OnGameReset();                        // the game's training reset (your controller's reset button)

    // every frame / every car input
    void OnFrame();
    void PollKeys();
    void Advance(float dt);
    void Apply();
    void OnCarInput(CarWrapper car, void* params);
    void ApplyCar(CarWrapper car, const State& s);
    Air AirAt(int k, float t) const;           // player k's jump and flip at moment t of the replay
    void ApplyAir(CarWrapper car, const Air& a);
    void Freeze(CarWrapper car);
    void Stash(CarWrapper car, bool away);
    void PaintCar(CarWrapper car, int k);
    void TrackCar(CarWrapper car, const State& s);   // a ghost of a simulation: by its speed, so it collides
    void Park(CarWrapper car);
    void SetGoals(bool enabled);

    // helpers
    State StateAt(int car, float t) const;     // car < 0: ball; mirrored when needed
    float Start() const { return replay ? replay->time.front() : 0.f; }
    float End() const { return replay ? replay->time.back() : 0.f; }
    float Now() const { return mode == MODE_SIM ? forkT + simT : rt; }
    int FrameAt(float t) const;
    int ToiIndex() const;
    int CarPlayer() const;                     // player your own car stands for: the one you drive (-1: you watch)
    bool Turned() const;
    int BotIndex(CarWrapper car) const;          // replay player this bot plays, PARKED, or -1
    std::string BotName(int k) const;          // player k's bot: his name (shown above the car)
    bool OurBot(CarWrapper car) const;         // a bot of ours: in freeplay every bot but the free camera's car
    std::string ClockAt(float t) const;
    float RemainingAt(float t) const;          // seconds left on the replay's clock, to the fraction
    bool OvertimeAt(float t) const;
    void ScoreAt(float t, int score[2]) const; // goals of each team at moment t (in a simulation: with its goal)
    float SimClock() const;                    // the simulation's clock: seconds left, or played in overtime
    std::string SimClockText() const;
    void UpdateMatch(BallWrapper ball);        // the clock reaching 0:00, the ball touching the ground, a goal
    void EndMatch(BallWrapper ball, const std::string& how);   // the final whistle: nobody drives, the ball goes
    void BallBack(ServerWrapper sw);           // the ball the end of a match stopped, back
    void DrawScoreboard(CanvasWrapper& canvas);
    void DrawCaptions(CanvasWrapper& canvas);  // a title and a caption for a video (rlab_titre, rlab_legende)
    std::string title[2], caption[2];          // first line, second line ("a | b")
    std::string BallText() const;              // what the ball does in the simulation, for the HUD
    void Status(const std::string& text, float seconds = 6.f);

    // cameras like in the replay viewer (Camera.cpp)
    void UpdateCamera();                       // every rendered frame
    void NextCamMode();
    void NextCamTarget();
    std::string CamText() const;
    CarWrapper CarOf(int k) const;             // the car on the pitch that stands for player k (null if none)
    void AimCamera();                          // just before the game works out its camera: the car it follows
    void ToggleBallCam(bool byYou = true);     // byYou: said on screen, and the followed player's ball cam left to you
    void CameraSettings(ServerWrapper sw);     // each bot: the camera settings of its player (the camera takes the followed car's)
    bool BallCamOn() const;                    // the camera's ball cam: the followed car's
    CarWrapper Followed() const;               // the car the camera follows
    bool FindBallCamFlag(CarWrapper car);      // where the ball cam flag sits in a camera settings actor
    bool BallCamOf(CarWrapper car) const;
    void SetBallCamOf(CarWrapper car, bool on);
    int ballCamByte = -1, ballCamBit = 0;      // that place (-2: not found)
    int ballCamWrites = 0;
    CarWrapper Dolly() const;                  // the invisible car the free camera follows
    int ShownMode() const;                     // the camera in use (the free one only when watching)
    int camMode = CAM_GAME;
    int camTarget = 0;
    Vector freePos{0, 0, 600};
    Rotator freeRot{0, 0, 0};
    bool dollyAsked = false;
    ControllerInput camInput{};                // your controller while it drives no car: flies the free camera
    std::chrono::steady_clock::time_point camSettingsCheck;
    int ballCamManual = -1;                    // player whose ball cam you switched (F8): his own is no longer followed
    std::chrono::steady_clock::time_point camLast, camShotStart, camBallCheck;

    // scenarios (Scenario.cpp): a simulation recorded tick by tick, to watch again like a replay
    void RecordFrame(ServerWrapper sw);
    void WatchScenario();
    void BackToOriginal();
    std::shared_ptr<ReplayData> original;      // the replay file that was opened
    std::shared_ptr<ReplayData> scenario;      // the simulation being recorded, or the last one
    std::vector<unsigned char> boostingNow;    // per player: boosting at its last input

    // drawing
    void DrawHud(CanvasWrapper canvas);
    void DrawCamera();
    void DrawWorld(CanvasWrapper& canvas);
    void DrawNameTags(CanvasWrapper& canvas);  // each player's name above his car, in his team's colour
    void DrawRoles();
    bool SetRole(int k, int role);
    void DrawOptions();

    // rlab_replay.dll
    using ParseFn = int (*)(const char*, unsigned char**, size_t*, char*, size_t);
    using FreeFn = void (*)(unsigned char*, size_t);
    void* reader = nullptr;
    ParseFn parseReplay = nullptr;
    ParseFn replayHeader = nullptr;
    FreeFn freeBlob = nullptr;

    std::recursive_mutex mtx;      // the window and loader threads read what the game thread writes
    std::vector<ReplayEntry> library;
    bool listing = false;
    bool loading = false;
    std::shared_ptr<ReplayData> replay;
    std::vector<int> roles;

    Mode mode = MODE_IDLE;
    bool playing = false;
    float playSpeed = 1.f;
    float rt = 0.f;                // replay time shown (absolute replay seconds)
    float forkT = 0.f;             // replay time where the simulation started
    float simT = 0.f;              // seconds simulated since the fork
    bool waiting = false;          // the simulation waits for you to move (like custom training)
    bool mirrored = false;
    bool ballLocked = false;
    float releaseT = -1.f;
    int goalTeam = -1;             // team that scored in the simulation (-1: no goal yet)
    float goalT = 0.f;
    int match = MATCH_PLAYING;
    std::string matchResult;       // shown once the match is over
    Vector ballEnd{0, 0, 0};       // where the ball stays once the match is over
    float clockAtFork = 0.f;       // seconds left on the clock (in overtime: played) when the simulation started
    float clockStop = -1.f;        // simT the clock stopped at (a goal, the end), -1 while it runs
    bool inOvertime = false;
    float overtimeT = 0.f;         // simT at which the overtime clock read +0:00 (before the fork if it started in overtime)
    static constexpr int PARKED = -2;
    static constexpr int DEAD = -3;      // a demolished car about to be taken away
    std::map<uintptr_t, int> botSlot;   // our bots by car address -> replay player (or PARKED)
    bool carsReady = false;
    float lastWorld = -1.f;        // game clock at the previous frame (-1: start again from the next frame)
    bool goalsOn = true;
    std::string origGoal, origGoalBm;   // freeplay goal settings to restore on unload
    int noDemo = -1;               // the game's demolition targets ("none", "other team"), read from its enum at load
    int normalDemo = -1;
    int freeplayBoost = 0;         // freeplay's unlimited boost count, given back to cars that follow the replay again
    std::vector<bool> demoed;      // players demolished in this simulation
    nexto::Model nextoModel;       // Nexto (plugins/replaylab/nexto.bin)
    std::vector<nexto::Action> nextoAction;   // what each Nexto car does until its next decision
    int nextoTick = 0;
    std::map<std::string, std::string> dressed;   // bot name -> look it wears (replay id/player)
    uintptr_t padServer = 0;       // boost pads seen picked up in this game: pad -> index in Nexto's order
    std::map<uintptr_t, int> pads;
    int debugFrame = 0;
    int spawning = -1;             // the player whose bot SpawnBot is making
    std::chrono::steady_clock::time_point redressedUntil;   // cars blown up to take a look are back by then
    std::map<uintptr_t, int> paintedAt;      // car -> tick it was last painted
    int applyTicks = 0;
    float lastApplyT = -1.f;
    std::vector<char> lastValid;             // per player: on the pitch in the replay at the last tick
    bool localStashed = false;     // your car hidden while you watch
    bool gameInFront = false;
    bool keyDown[8] = {};          // F3 F4 F5 F7 F8 F9 F10 F11 at the last frame
    int padKey[5] = {-1, -1, -1, -1, -1};   // the controller's key names in the game: d-pad up, left, right, down, simulate
    bool padDown[5] = {};
    std::string padSimulate;       // the key name padKey[4] was looked up for (rlab_manette_simuler)
    int rolePlayer = -1, roleNext = -1;  // role picked with the d-pad, applied once you stop pressing
    std::chrono::steady_clock::time_point roleAt, lastReset;
    bool tickSeen = false;         // a physics tick came through your car since the last rendered frame
    float noTickTime = 0.f;
    float gapSum = 0.f;            // rlab_debug: how far physics took your car from the replay between two ticks
    int gapN = 0;
    int burst = 0;
    void CarRules(CarWrapper car, bool drivesItself);
    int RoleOf(int k) const;               // role in the simulation going on (a demolished ghost: a bot)
    void OnDemolished(CarWrapper car);
    void MarkDemolished(int k);
    bool DrivesItself(int k) const;        // a bot, Nexto or a free car: not the replay, not you
    void NextoDecide(ServerWrapper sw, CarWrapper local);
    void NextoInput(int k, ControllerInput* in) const;
    void OnPadPickedUp(ActorWrapper pad);
    void PadStates(ServerWrapper sw, float pads[nexto::PADS]);
    void DemolishPlayer(int k);

    bool windowOpen = false;
    std::string status;
    std::chrono::steady_clock::time_point statusUntil;
};
