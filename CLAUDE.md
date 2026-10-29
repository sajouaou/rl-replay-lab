# RLReplayLab: briefing for AI assistants

Rocket League replay tools. The main one is **Replay Lab**, a BakkesMod plugin (`bakkesmod/`) that replays a `.replay`
inside the game's free play like the real match and lets the player take over at any moment (simulations, recorded as
branches). The web lab (`lab/`, `static/`, Python + RocketSim + three.js) is older and separate: do not touch it when
the task is about the plugin.

Read before working: `docs/OVERVIEW.md` (concepts, glossary), `docs/ARCHITECTURE.md` (files, blob format, data flow,
match flow, threads, memory), `docs/DEVELOPING.md` (build, checks, commands, conventions, pitfalls). The player's guide
is `bakkesmod/README.md` (French); keep it up to date with behaviour changes. Testing notes and the last report:
`NOTES.md`, `REPORT.md`.

## Layout

- `bakkesmod/ReplayLab/` the plugin, C++17, MSVC ABI. `ReplayLab.h` declares everything, members grouped by the file
  that implements them: `ReplayLab.cpp` (entry, settings, hooks), `Input.cpp`, `ReplayFile.cpp`, `Scene.cpp` (bots),
  `Playback.cpp` (per-tick loop), `Match.cpp` (match-like playback), `Simulation.cpp`, `Branches.cpp`, `Camera.cpp`,
  `Hud.cpp`, `Window.cpp`; shared helpers in `Rules.h`, `LabMath.h`, `Draw.h`.
- `bakkesmod/rlab_replay/` the replay reader (Rust, subtr-actor), C ABI DLL; `examples/flow.rs` shows a replay's match
  flow. The blob format is documented at the top of `src/lib.rs`.
- `bakkesmod/nexto/` the Nexto bot's network in C++.

## Build and verify

- `cd bakkesmod && ./build.sh` builds both DLLs from Linux (clang-cl + xwin + lld-link; cargo inside a systemd memory
  scope: this machine has little free memory while the game runs). `./install_plugin.sh` installs them.
- Fast check: the `clang-cl ... /Zs -W4 ... ReplayLab/*.cpp` command in `docs/DEVELOPING.md`; only the deprecated
  `SetBotLoadout` warning is expected.
- Offline checks of replay data: `cargo run --release --example flow|dump -- <replay>` (natively, in the memory scope).
- Replays live in the Proton prefix: `…/compatdata/252950/pfx/drive_c/users/steamuser/Documents/My Games/Rocket League/TAGame/Demos`.

## Conventions

- Comments: short English sentences explaining why, matching the existing voice. In-game/console strings: French
  without accents (the game's font has none).
- Read settings through `opt` (bound once in `BindSettings`), never `getCvar(...).getXValue()` in a tick.
- Shared state under `mtx`; the ImGui window queues game actions with `gameWrapper->Execute`.
- The blob only grows: append fields with a new version, keep `Decode` reading older ones.

## Never

- Never inject BakkesMod (`lancer_bakkesmod.sh`) or drive the user's game without being asked: the game must have been
  started without Easy Anti-Cheat, and the session is the user's.
- Never remove bots from freeplay with `RemovePlayer` casually (it crashed the game); reuse them.
- Never spawn several bots in one frame; never create/destroy actors inside a physics step when `OnFrame` can do it.
- Never run cargo outside the memory scope on this machine; never commit or push unless the user asks.
