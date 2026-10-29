# RLReplayLab: overview (the fundamental level)

Read this first. It explains what the project is and the ideas everything else builds on. The technical level is in
[ARCHITECTURE.md](ARCHITECTURE.md), the practical guide in [DEVELOPING.md](DEVELOPING.md). The player's guide (French)
is [bakkesmod/README.md](../bakkesmod/README.md).

## What it is

Two tools to replay Rocket League replays and play the actions differently ("what if"):

- **Replay Lab, the BakkesMod plugin** (`bakkesmod/`), the main tool. Inside the game's free play it replays a
  `.replay` file like the real match (its stadium and mode, each player in his own car, kickoffs, goals that explode,
  goal replays, clock, score), and at any moment you can **take over**: the game's real physics continues from there,
  with you, bots, or the Nexto AI driving instead of the real players. Each of these simulations is kept as a
  **branch**, which you can watch again like a replay, film, and simulate from again.
- **The web lab** (`lab/`, `static/`): the same idea in a browser, with the RocketSim physics engine (Python server and
  three.js viewer). It is older and separate; the plugin does not depend on it.

## The core ideas

**A replay records where things were, not what players pressed.** A `.replay` file is the network stream of the match:
positions, rotations and velocities of the ball and cars about 30 times a second (each car every 2-3 frames, at uneven
moments), plus game events (goals, demolitions, the clock, the countdown). The plugin reads it with the Rust library
subtr-actor (through `rlab_replay.dll`) and turns it into one binary blob per replay.

**Playing a replay = putting everything where it was, 120 times a second.** BakkesMod lets a plugin run code inside the
game. The plugin loads free play in the replay's stadium, spawns one **bot** per replay player, dresses it with that
player's car and items (read from the replay), and at every physics tick of the game teleports each car and the ball to
their recorded place (smoothed so they move fluidly). The game draws them, so it looks like the real thing.

**Taking over = forking.** "Simuler d'ici" (simulate from here) is a fork: at that instant each car gets a **role** and
the game's physics stops being overridden for those that are no longer following the replay:

| Role (French UI) | Meaning |
| --- | --- |
| Toi | you drive this player's car with your controller |
| Fantome (ghost) | keeps following the real path, but physically (it can still bump and touch the ball) |
| Retiree (removed) | the player is taken out |
| Bot Psyonix | the game's own AI drives it |
| Nexto | a neural-network bot from RLGym, run inside the plugin |
| Libre (free) | nobody drives; the car just coasts |

The ball keeps following the replay until a changed car touches it (or, in the replay, a changed player did), then it
is free too. A simulation follows a real
match's rules: the clock runs, a goal stops it, at 0:00 the match ends when the ball touches the ground, a tie goes to
overtime.

**Tries and branches.** Every simulation is recorded (60 times a second) into the same structure as a replay, so it can
be watched like a replay (cameras, slow motion, cinema mode) and forked again. A try is not kept: the next one replaces
it, so the player can retry as often as he wants. When he saves it, it becomes a **branch**: the original replay and
its saved branches form a tree of "what if"s (the last 8 saved are kept).

**Like the real match.** A replay file has holes and resets that a naive player gets wrong. After each goal: about 3 s of
play, then a hole in time (the game was showing its goal replay, which is not recorded), then every car and a new ball
recreated at the kickoff spots, then the 3-2-1 countdown. The plugin reads the game's state for each frame (countdown,
playing, goal) and reproduces what the game showed: the ball exploding with the scorer's goal explosion, the scorer's
banner with the ball speed, its own goal replay in the hole, the kickoff countdown, demolitions at the right moment.

## Glossary

| Term | Meaning |
| --- | --- |
| BakkesMod | community mod loader for Rocket League; plugins are C++ DLLs using its SDK (`bakkesmod/sdk`) |
| freeplay | the game's free play training mode: the only offline mode where a plugin can spawn bots and move anything |
| replay | a `.replay` file from `Documents/My Games/Rocket League/TAGame/Demos` |
| frame | one recorded moment of a replay (~30 per second) |
| tick | one physics step of the game: 120 per second of game time |
| blob | the binary form of a replay the Rust reader hands to the plugin ("RLAB", versions 1-5) |
| phase | what the match is doing at a frame: kickoff countdown, playing, goal (from the explosion to the next kickoff) |
| hole / cut | the jump in time after a goal, where the game showed its goal replay |
| fork | starting a simulation at a moment of the replay ("Simuler d'ici") |
| ghost | a car that keeps following the replay during a simulation |
| branch (scenario) | a recorded simulation, watchable and forkable like a replay |
| PRI | PlayerReplicationInfo: the game's object for a player (name, team, loadout); a goal explosion is chosen from the scorer's |
| Nexto | an RLGym bot (Rolv-Arild and Soren, CC BY-NC-SA 4.0) whose network the plugin runs in C++ |
| subtr-actor, boxcars | Rust crates that parse replays |
| RocketSim | a standalone Rocket League physics engine (used by the web lab only) |

## Where to start

1. Use it: `bakkesmod/README.md` (install, keys, controller).
2. See how a replay plays its match: `cargo run --release --example flow -- <replay>` in `bakkesmod/rlab_replay`.
3. Read the plugin in this order: `ReplayLab.h` (all state, grouped by file), `Playback.cpp` (the per-tick loop),
   `Match.cpp` (the match-like playback), `Simulation.cpp`, `Branches.cpp`.
