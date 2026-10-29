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
#include "bakkesmod/core/bot_loadout_structs.h"
#include "nexto.h"

#include <array>
#include <chrono>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
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
    float throttle = 0.f, steer = 0.f;   // -1..1, the player's sticks: his car's wheels turn and spin like his
    bool slide = false;             // powersliding
    bool valid = false;
};

// what a player's controller does at a tick, kept for the branch being recorded
struct Sticks {
    unsigned char throttle = 128, steer = 128;   // 0-255, 128: none (as replays store them)
    bool boosting = false, slide = false;
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

// how the match goes at a frame of a replay: the kickoff countdown, play, a goal (from its explosion to the next kickoff)
enum Phase { PHASE_OTHER, PHASE_COUNTDOWN, PHASE_PLAYING, PHASE_GOAL };

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
    std::vector<unsigned char> moves;           // same indexing: 1 jumping, 2 double jumping, 4 dodging, 8 powersliding
    std::vector<unsigned char> throttle, steer; // same indexing: the player's sticks (0-255, 128: none; empty: unknown)
    int baseScore[2] = {0, 0};                  // goals before the first frame (a scenario, a match joined late)
    std::vector<std::pair<int, int>> goals;     // (frame, scoring team)
    std::vector<std::pair<int, int>> scorers;   // [goal] (scorer, assist): players, -1 for none
    std::vector<std::pair<int, int>> saves;     // (frame, player)
    std::vector<std::pair<int, int>> demos;     // (frame, demolished player)
    bool demosKnown = false;                    // demolitions listed: a car missing otherwise was not demolished
    std::vector<unsigned char> phase;           // [frame] Phase (empty: unknown, a scenario)
    std::vector<unsigned char> countdown;       // [frame] the kickoff countdown shown: 3, 2, 1 (0: none)
    std::vector<int> cuts;                      // frames after which a goal's phase jumps to its kickoff (sorted)
    struct PadEvent {
        float x, y, time;
        bool taken;                             // picked up; false: back
    };
    std::vector<PadEvent> pads;                 // the boost pads taken and back, in time order (version 6)
    std::vector<Look> looks;                    // [player]
    std::vector<PlayerCam> cams;                // [player]
    bool scenario = false;                      // a simulation recorded in the game (a branch), not a replay file
    std::string label;                          // a branch: its number, where it started, how it ended
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

// a goal shown again right after it, like the game's replay of a goal
struct GoalReplay {
    bool on = false;
    int goal = -1;                 // in replay->goals
    float from = 0.f, to = 0.f;    // what it shows: a few seconds before the goal to just after it
    float resume = -1.f;           // where the match goes on after it (the kickoff); -1: it stops there (a simulation's goal)
    float matchT = 0.f;            // the moment whose score and clock show meanwhile
    bool camSet = false;           // the camera put on the scorer; yours, back after it:
    int camMode = 0, camTarget = 0, ballCamManual = -1;
};

struct ReplayEntry {
    std::string id, path, map, date, label;
};

enum Role { ROLE_TOI, ROLE_GHOST, ROLE_REMOVED, ROLE_BOT, ROLE_FREE, ROLE_NEXTO, ROLE_COUNT };
enum Mode { MODE_IDLE, MODE_PLAYBACK, MODE_SIM };
enum CamMode { CAM_GAME, CAM_PLAYER, CAM_FREE, CAM_AUTO, CAM_COUNT };

// The settings: the console variables rlab_*, bound once in onLoad (BindSettings) so they are read without looking
// them up by name (several are read every physics tick). Written through the console variable, the value follows.
struct Settings {
    std::shared_ptr<bool> manette = std::make_shared<bool>(true);   // rlab_manette
    std::shared_ptr<bool> tableau = std::make_shared<bool>(true);   // rlab_tableau
    std::shared_ptr<bool> cinema = std::make_shared<bool>(false);   // rlab_cinema
    std::shared_ptr<bool> camJoueur = std::make_shared<bool>(true);   // rlab_cam_joueur
    std::shared_ptr<bool> sync = std::make_shared<bool>(true);   // rlab_sync
    std::shared_ptr<bool> noms = std::make_shared<bool>(true);   // rlab_noms
    std::shared_ptr<bool> nomsPlugin = std::make_shared<bool>(false);   // rlab_noms_plugin
    std::shared_ptr<bool> mirror = std::make_shared<bool>(false);   // rlab_mirror
    std::shared_ptr<bool> ghosts = std::make_shared<bool>(true);   // rlab_ghosts
    std::shared_ptr<bool> explosion = std::make_shared<bool>(true);   // rlab_explosion
    std::shared_ptr<bool> voitureJoueur = std::make_shared<bool>(true);   // rlab_voiture_joueur
    std::shared_ptr<bool> revoirSimulation = std::make_shared<bool>(true);   // rlab_revoir_simulation
    std::shared_ptr<bool> revoirButs = std::make_shared<bool>(true);   // rlab_revoir_buts
    std::shared_ptr<int> attendre = std::make_shared<int>(1);   // rlab_attendre
    std::shared_ptr<int> debug = std::make_shared<int>(0);   // rlab_debug
    std::shared_ptr<float> speed = std::make_shared<float>(1.f);   // rlab_speed
    std::shared_ptr<float> gamma = std::make_shared<float>(1.f);   // rlab_gamma
    std::shared_ptr<std::string> player = std::make_shared<std::string>("Anxoide");   // rlab_player
    std::shared_ptr<std::string> manetteSimuler = std::make_shared<std::string>("XboxTypeS_Back");   // rlab_manette_simuler
};

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
    /* ---------------------------------------------------------- ReplayLab.cpp: the plugin's entry */
    Settings opt;
    void BindSettings();
    bool LoadReader();
    std::wstring DemosFolder() const;
    void Status(const std::string& text, float seconds = 6.f);
    std::recursive_mutex mtx;      // the window and loader threads read what the game thread writes
    std::string status;
    std::chrono::steady_clock::time_point statusUntil;
    std::string origGoal, origGoalBm;   // freeplay goal settings to restore on unload
    int noDemo = -1;               // the game's demolition targets ("none", "other team"), read from its enum at load
    int normalDemo = -1;
    std::string title[2], caption[2];          // a title and a caption for a video (rlab_titre, rlab_legende): "a | b"
    std::chrono::steady_clock::time_point flashUntil;   // rlab_flash: the whole screen magenta, a mark to find in a video
    // rlab_replay.dll
    using ParseFn = int (*)(const char*, unsigned char**, size_t*, char*, size_t);
    using FreeFn = void (*)(unsigned char*, size_t);
    void* reader = nullptr;
    ParseFn parseReplay = nullptr;
    ParseFn replayHeader = nullptr;
    FreeFn freeBlob = nullptr;

    /* ---------------------------------------------------------- Input.cpp: keys and controller */
    void PollKeys();
    void NextRole();                           // controller: the next role for the player in view (applied after a pause)
    void OnGameReset();                        // the game's training reset (your controller's reset button)
    ControllerInput Pad() const;               // your controller while it drives no car (nothing if not read lately)
    ControllerInput camInput{};                // your controller while it drives no car: flies the free camera
    std::chrono::steady_clock::time_point camInputAt;    // camInput read then (older: your controller is not read)
    bool gameInFront = false;
    std::chrono::steady_clock::time_point focusWritten;   // data/replaylab_focus.txt: for a script filming the game
    bool keyDown[8] = {};          // F3 F4 F5 F7 F8 F9 F10 F11 at the last frame
    int padKey[6] = {-1, -1, -1, -1, -1, -1};   // the controller's key names in the game: d-pad up, left, right, down, simulate, A
    bool padDown[6] = {};
    bool padLong[6] = {};                   // held long enough for its second action, done
    std::chrono::steady_clock::time_point padSince[6];   // pressed since
    std::string padSimulate;       // the key name padKey[4] was looked up for (rlab_manette_simuler)
    int rolePlayer = -1, roleNext = -1;  // role picked with the d-pad, applied once you stop pressing
    std::chrono::steady_clock::time_point roleAt, lastReset;

    /* ---------------------------------------------------------- ReplayFile.cpp: replay files */
    void RefreshList();
    void OpenReplay(const std::string& id, int startFrame, bool simulate, const std::string& followed);
    void OpenFromViewer();
    bool Decode(const unsigned char* data, size_t len, ReplayData& out) const;
    void RunWorker(std::thread& worker, std::function<void()> work);
    std::function<void()> opened;              // a replay just read, for the game's next frame (TakeOpened)
    void TakeOpened();
    std::thread listWorker, openWorker;        // reading the replay list, reading a replay
    std::vector<ReplayEntry> library;
    bool listing = false;
    bool loading = false;
    std::shared_ptr<ReplayData> replay;        // what plays: the replay file, or a branch
    std::shared_ptr<ReplayData> original;      // the replay file that was opened

    /* ---------------------------------------------------------- Scene.cpp: freeplay and its bots */
    void EnterFreeplay(std::function<void()> then);
    bool InReplayStadium() const;              // the replay's map, in the replay's mode
    int GameKind() const;                      // the replay's mode
    void WaitForFreeplay(int tries, std::function<void()> then);
    int ToiIndex() const;
    int CarPlayer() const;                     // player your own car stands for: the one you drive (-1: you watch)
    bool Driving() const;                      // in a simulation you drive the player's own car (his bot)
    bool Turned() const;
    bool NeedsBot(int k, bool forSim) const;
    void SetupCars(std::function<void()> then, bool forSim);
    void SpawnBots(std::vector<int> missing, int tries, std::function<void()> then, bool forSim);
    void MapBots(bool forSim);
    int BotIndex(CarWrapper car) const;          // replay player this bot plays, PARKED, or -1
    std::string BotName(int k) const;          // player k's bot: his name (shown above the car)
    bool OurBot(CarWrapper car) const;         // a bot of ours: in freeplay every bot but the free camera's car
    void DressCars();                          // each car: the team of the player it stands for
    void OnSpawnCar();                         // a bot of ours being made: the look and team of its player, before its car
    void Dress(PriWrapper pri, int k);         // a bot's player info: the look and team of player k
    BotLoadoutData LoadoutOf(int k) const;     // player k's look for SetBotLoadout
    void Redress(CarWrapper car, int k);       // ... then its car blown up, to come back with them
    int GameTeam(int k) const;                 // team in the game (the replay's, swapped if the scene is turned around)
    void PaintCar(CarWrapper car, int k);
    void Freeze(CarWrapper car);
    void Stash(CarWrapper car, bool away);
    void Park(CarWrapper car);
    void SetGoals(bool enabled);
    void CarRules(CarWrapper car, bool drivesItself);
    static constexpr int PARKED = -2;
    static constexpr int DEAD = -3;      // a demolished car about to be taken away
    std::map<uintptr_t, int> botSlot;   // our bots by car address -> replay player (or PARKED)
    std::vector<int> roles;
    bool carsReady = false;
    bool goalsOn = true;
    int freeplayBoost = 0;         // freeplay's unlimited boost count, given back to cars that follow the replay again
    std::map<std::string, std::string> dressed;   // bot name -> look it wears (replay id/player)
    int spawning = -1;             // the player whose bot SpawnBot is making
    std::chrono::steady_clock::time_point redressedUntil;   // cars blown up to take a look are back by then
    bool ownCar = false;                       // you drive your own car, not the player's bot (rlab_voiture_joueur 0)
    ControllerInput myInput{};                 // your controller this tick, for the bot you drive
    std::map<uintptr_t, int> paintedAt;      // car -> tick it was last painted
    bool localStashed = false;     // your car hidden while you watch

    /* ---------------------------------------------------------- Playback.cpp: every tick, everything in its place */
    void PlayFrom(float t, bool play);
    void StartPlayback(float t, bool play);
    void TogglePlay();
    void Seek(float t);
    void OnCarInput(CarWrapper car, void* params);
    void OnFrame();
    void Advance(float dt);
    void Apply();
    void ApplyCar(CarWrapper car, const State& s);
    void TrackCar(CarWrapper car, const State& s);   // a ghost of a simulation: by its speed, so it collides
    State StateAt(int car, float t) const;     // car < 0: ball; mirrored when needed
    float Start() const { return replay ? replay->time.front() : 0.f; }
    float End() const { return replay ? replay->time.back() : 0.f; }
    float Now() const { return mode == MODE_SIM ? forkT + simT : rt; }
    int FrameAt(float t) const;
    Air AirAt(int k, float t) const;           // player k's jump and flip at moment t of the replay
    void ApplyAir(CarWrapper car, const Air& a);
    std::string ClockAt(float t) const;
    float RemainingAt(float t) const;          // seconds left on the replay's clock, to the fraction
    bool OvertimeAt(float t) const;
    void ScoreAt(float t, int score[2]) const; // goals of each team at moment t (in a simulation: with its goal)
    Mode mode = MODE_IDLE;
    bool playing = false;
    float playSpeed = 1.f;
    float rt = 0.f;                // replay time shown (absolute replay seconds)
    bool mirrored = false;
    float lastWorld = -1.f;        // game clock at the previous frame (-1: start again from the next frame)
    int applyTicks = 0;
    float lastApplyT = -1.f;
    std::vector<char> lastValid;             // per player: on the pitch in the replay at the last tick
    std::vector<char> onPitch;              // per player: his car on the pitch at this tick (kept: no allocation per tick)
    bool tickSeen = false;         // a physics tick came through your car since the last rendered frame
    float noTickTime = 0.f;
    int debugFrame = 0;
    float gapSum = 0.f;            // rlab_debug: how far physics took your car from the replay between two ticks
    int gapN = 0;
    int burst = 0;

    /* ---------------------------------------------------------- Simulation.cpp: simulating from a moment */
    void Fork();
    void PlaceAtFork(bool moving);               // everything as at the moment the simulation starts from
    void BackToReplay();
    void OnSimulateKey();
    void NextView();
    int RoleOf(int k) const;               // role in the simulation going on (a demolished ghost: a bot)
    bool DrivesItself(int k) const;        // a bot, Nexto or a free car: not the replay, not you
    void DemolishPlayer(int k);
    void OnDemolished(CarWrapper car);
    void MarkDemolished(int k);
    float SimClock() const;                    // the simulation's clock: seconds left, or played in overtime
    std::string SimClockText() const;
    void UpdateMatch(BallWrapper ball);        // the clock reaching 0:00, the ball touching the ground, a goal
    void EndMatch(BallWrapper ball, const std::string& how);   // the final whistle: nobody drives, the ball goes
    void BallBack(ServerWrapper sw);           // the ball the end of a match stopped or a goal blew up, back
    std::string BallText() const;              // what the ball does in the simulation, for the HUD
    void NextoDecide(ServerWrapper sw, CarWrapper local);
    void NextoInput(int k, ControllerInput* in) const;
    void OnPadPickedUp(ActorWrapper pad);
    void NotePad(ActorWrapper pad);            // a boost pad of this game: where it is
    struct KnownPad {
        Vector at;
        float delay;                           // its own respawn delay (4 s small, 10 s big)
        std::vector<int> events;               // its events in the replay that plays (replay->pads)
    };
    void PadEvents();                          // each known pad's events, listed again for another replay
    bool PadTaken(const KnownPad& pad, float t, float* back) const;   // taken at t in the replay; when it is back
    void PadsAtFork(ServerWrapper sw);         // each pad as it was in the replay when the simulation starts
    void UpdatePads();                         // the pads taken then, back when they were in the replay
    void SyncPads(float t);
    void RecordPad(ActorWrapper pad, bool taken);   // a pad taken or back in the try being recorded                    // while you watch: each pad as the replay has it at t
    void PadDelaysBack();                      // the pads' own respawn delays, back
    static constexpr float PAD_HELD = 100000.f;   // a respawn delay that never comes: the plugin brings the pad back
    std::map<uintptr_t, KnownPad> padPlaces;   // the pads seen in this game (picked up or respawned)
    std::weak_ptr<const ReplayData> padsOf;    // the replay whose events padPlaces lists
    bool padsMirrored = false;
    std::vector<std::pair<uintptr_t, float>> padReturns;   // pads taken at the fork: back at that simT
    void PadStates(ServerWrapper sw, float pads[nexto::PADS]);
    float forkT = 0.f;             // replay time where the simulation started
    float simT = 0.f;              // seconds simulated since the fork
    bool waiting = false;          // the simulation waits for you to move (like custom training)
    float kickoff = 0.f;           // a simulation starting with a kickoff countdown: game seconds left before GO
    bool ballLocked = false;       // the ball still follows the replay
    int goalTeam = -1;             // team that scored in the simulation (-1: no goal yet)
    float goalT = 0.f;
    int match = MATCH_PLAYING;
    std::string matchResult;       // shown once the match is over
    Vector ballEnd{0, 0, 0};       // where the ball stays once the match is over
    float clockAtFork = 0.f;       // seconds left on the clock (in overtime: played) when the simulation started
    float clockStop = -1.f;        // simT the clock stopped at (a goal, the end), -1 while it runs
    bool inOvertime = false;
    float overtimeT = 0.f;         // simT at which the overtime clock read +0:00 (before the fork if it started in overtime)
    std::vector<bool> demoed;      // players demolished in this simulation
    std::vector<bool> bumped;      // ghosts a car that drives itself came against: free from then on
    bool replayScored = false;     // the replay scored a goal the simulation did not: no more ghosts
    std::vector<Vector> freeCars;  // where the cars that drive themselves are, this tick
    bool Bumped(CarWrapper car, int k);   // a ghost a free car comes against: let go
    static constexpr float BUMP_DIST = 230.f;   // between two cars' centres: about to touch
    nexto::Model nextoModel;       // Nexto (plugins/replaylab/nexto.bin)
    std::vector<nexto::Action> nextoAction;   // what each Nexto car does until its next decision
    int nextoTick = 0;
    uintptr_t padServer = 0;       // boost pads seen picked up in this game: pad -> index in Nexto's order
    std::map<uintptr_t, int> pads;

    /* ---------------------------------------------------------- Match.cpp: the replay played like the match */
    int PhaseAt(float t) const;                // Phase of the replay at moment t (PHASE_OTHER when unknown)
    bool HoleAfter(int frame) const;           // the jump from a goal's phase to its kickoff happens after this frame
    int GoalBefore(int frame) const;           // the last goal at or before this frame (-1: none)
    float NextKickoff(float t) const;          // the moment play starts again (GO!), or t
    bool BallGoneAt(float t) const;            // blown up by a goal at moment t (until the kickoff)
    float FadeAt(float t) const;               // 0..1: the screen going dark around a jump in time
    float ShownT() const;                      // the moment the scoreboard shows (during a goal's replay: after the goal)
    void AdvancePlayback(float dt);            // playback time: each goal's replay or a jump to its kickoff, then the kickoff
    void StartGoalReplay(int goal, float resume, float matchT);
    void EndGoalReplay();                      // over: on to the kickoff (a simulation's goal: it stays there)
    void CancelGoalReplay();                   // stopped where it is
    void GoalTeamBack(ServerWrapper sw);       // the net an explosion borrowed, freeplay's again
    void ReplaySimGoal();                      // a simulation's goal shown again, from its recording
    void UpdateSimGoal();                      // a moment after a simulation's goal: shown again (rlab_revoir_simulation)
    void ReplayEvents(ServerWrapper sw, float from, float to);   // the replay's goals, demolitions, countdown in between
    void ExplodeBall(ServerWrapper sw, int team, int scorer, Vector at);   // the ball blows up with the scorer's goal explosion
    Vector GoalSpot(int goal) const;           // where the ball went in
    PriWrapper PriOf(int k) const;             // the player info that stands for player k: his bot's (yours if you play him)
    void ShowBall(ServerWrapper sw, bool shown);   // the ball gone after a goal, back for the kickoff (a new one if blown up)
    void RenewBall(ServerWrapper sw);          // that new one, between two frames
    void OnHitBall(CarWrapper car);            // in a simulation: the last player of each team to touch the ball
    int SimGoalScorer(int team) const;         // who scored the simulation's goal: the last of his team to touch the ball
    void CountdownMessage(int n);              // the game's own kickoff countdown: 3, 2, 1; 0: GO!
    void DrawMatch(CanvasWrapper& canvas);     // who scored, a goal's replay, the fade, the end of the match
    void DrawGoal(CanvasWrapper& canvas, int team, int scorer, int assist, float kmh, float since);
    void DrawGoalReplay(CanvasWrapper& canvas);
    float GoalKmh(int goal) const;             // how fast the ball went in (km/h)
    std::string PlayerText(int k) const;       // a player's name as the game's font can write it
    GoalReplay goalReplay;
    bool ballGone = false;                     // the ball hidden by us after a goal
    bool ballBlown = false;                    // ... blown up by the game: it does not play again, a new one comes
    bool ballRenew = false;                    // a new ball wanted
    std::pair<uintptr_t, unsigned char> goalTeamBack{0, 0};   // a goal whose team an explosion changed: its own, next frame
    int lastTouch[2] = {-1, -1};               // per team (the replay's), the last player who touched the ball in a simulation
    int goalScorer = -1;                       // the simulation's goal: who scored it, how fast
    float goalKmh = 0.f;
    bool goalReplayed = false;                 // the simulation's goal already shown again

    /* ---------------------------------------------------------- Branches.cpp: simulations kept, a tree of "what if"s */
    void RecordFrame(ServerWrapper sw);
    void SaveBranch();                         // the last try kept as a branch (only when you decide)
    void WatchBranch(int i);                   // branch i (-1: the last try, saved or not)
    void NextBranch();                         // the original replay, then each branch from the last one, and around
    void BackToOriginal();
    void Retry();                              // the last simulation again, from the same moment of what it started from
    std::shared_ptr<ReplayData> scenario;      // the try being recorded, or the last one (a branch once saved)
    std::shared_ptr<ReplayData> simSource;     // what the last simulation was started from: the replay or a branch
    std::vector<std::shared_ptr<ReplayData>> branches;   // the saved simulations, oldest first
    int branchCount = 0;                       // branches saved from this replay (their numbers)
    std::vector<Sticks> inputsNow;             // per player: his last input, recorded in the branch

    /* ---------------------------------------------------------- Camera.cpp: cameras like the replay viewer's */
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
    CarWrapper Dolly() const;                  // the invisible car the free camera follows
    int ShownMode() const;                     // the camera in use (the free one only when watching)
    int ballCamByte = -1, ballCamBit = 0;      // that place (-2: not found)
    int ballCamWrites = 0;
    int camMode = CAM_GAME;
    int camTarget = 0;
    Vector freePos{0, 0, 600};
    Rotator freeRot{0, 0, 0};
    bool dollyAsked = false;
    std::chrono::steady_clock::time_point camSettingsCheck;
    int ballCamManual = -1;                    // player whose ball cam you switched (F8): his own is no longer followed
    std::chrono::steady_clock::time_point camLast, camShotStart, camBallCheck;

    /* ---------------------------------------------------------- Hud.cpp: drawn over the game */
    void DrawHud(CanvasWrapper canvas);
    void DrawScoreboard(CanvasWrapper& canvas);
    void DrawNameTags(CanvasWrapper& canvas);  // each player's name above his car, in his team's colour
    void DrawCaptions(CanvasWrapper& canvas);  // a title and a caption for a video (rlab_titre, rlab_legende)
    void DrawWorld(CanvasWrapper& canvas);

    /* ---------------------------------------------------------- Window.cpp: the F3 window */
    void DrawCamera();
    void DrawRoles();
    bool SetRole(int k, int role);
    void DrawOptions();
    void DrawBranches();
    bool windowOpen = false;
};
