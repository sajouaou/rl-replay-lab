# Developing the plugin

## Build and install (Linux, cross-compiled for Windows)

    cd bakkesmod
    ./build.sh            # rlab_replay.dll (cargo, inside a 1.5 GB systemd scope) then ReplayLab.dll (clang-cl + lld-link)
    ./install_plugin.sh   # copies both DLLs and nexto.bin into the Proton prefix's bakkesmod/plugins (safe while the game runs)

First time on a new machine: see "Recompiler" in `bakkesmod/README.md` (clang, Rust with the `x86_64-pc-windows-msvc`
target, xwin for the Microsoft headers, `fix_sdk_case.py` for the SDK's file names, `RL_PREFIX`).

Then in the game: launch Rocket League **without Easy Anti-Cheat**, run `./lancer_bakkesmod.sh` at the menu, and after an
update type in the BakkesMod console (F6) `plugin unload replaylab` then `plugin load replaylab`.

## Checking without the game

- Syntax and warnings of every file, fast:

      clang-cl --target=x86_64-pc-windows-msvc /nologo /std:c++17 /EHsc /MD /utf-8 /Zs -W4 \
        -Wno-unused-command-line-argument -Wno-ignored-pragmas -Wno-microsoft-template -Wno-nonportable-include-path \
        -Wno-reorder-ctor /imsvc toolchain/msvc/crt/include /imsvc toolchain/msvc/sdk/include/ucrt \
        /imsvc toolchain/msvc/sdk/include/um /imsvc toolchain/msvc/sdk/include/shared \
        /I sdk/include /I imgui_src /I nexto ReplayLab/*.cpp

  The only expected warning is `SetBotLoadout` being deprecated (it is what dresses the bots).
- How a replay plays its match (states, countdown, goals, demolitions, holes), natively:

      cd rlab_replay
      cargo run --release --example flow -- <replay>          # FLOW_CARS=1: every frame; FLOW_SUMMARY=1: one line
      cargo run --release --example dump -- <replay> out.blob # the blob the plugin receives

  Run cargo inside the same memory cap as `build.sh` on this machine
  (`systemd-run --user --scope -p MemoryMax=1500M -p MemorySwapMax=0 cargo ... -j1`).
- Anything touching the game's objects (bots, the ball, the camera, explosions) needs the game.

## In the game

| Command / variable | Use |
| --- | --- |
| `rlab_etat` | prints the plugin's state: mode, time, cars, ball, goals, camera, keys |
| `rlab_debug 1` (2: every tick) | where each car should be vs where it is, boost, demolition targets |
| `rlab_open <id> [frame] [1]` | open a replay by file id, at a frame, simulating right away |
| `rlab_role <player> <role>` | 0 you, 1 ghost, 2 removed, 3 Psyonix bot, 4 free, 5 Nexto |
| `rlab_play`, `rlab_retry`, `rlab_capture` | simulate here (F5), retry (F11), play/pause (F9) |
| `rlab_sauver`, `rlab_revoir [n]`, `rlab_branche`, `rlab_origine` | save the last try as a branch, watch (n: a saved branch; none: the last try), next branch, back to the original replay |
| `rlab_camera`, `rlab_cam_mode [n]`, `rlab_ballcam` | next player, camera mode, ball cam |
| `rlab_ralenti`, `rlab_vitesse <x>`, `rlab_cinema_bascule` | slow motion cycle, playback speed, cinema mode |
| `rlab_demolir <player>` | demolish a player now, in a simulation |
| `rlab_titre`, `rlab_legende`, `rlab_flash`, `rlab_hud`, `rlab_photo` | video helpers: title, caption, magenta mark, game HUD, screenshot |
| `rlab_explosion`, `rlab_revoir_buts`, `rlab_revoir_simulation`, `rlab_attendre` | goals explode; goal replays; a simulation's goal replay; how a simulation starts (0 at once, 1 when you move, 2 countdown) |
| `rlab_tableau`, `rlab_noms`, `rlab_cinema`, `rlab_manette`, `rlab_manette_simuler` | scoreboard and match texts, name tags, cinema mode, controller, the controller's simulate button |
| `rlab_sync`, `rlab_ghosts`, `rlab_mirror`, `rlab_speed`, `rlab_gamma`, `rlab_cam_joueur`, `rlab_player` | ball follows the replay, wireframes, turn the scene, simulation game speed, paint gamma, players' camera settings, your name |

The BakkesMod log is `…/compatdata/252950/pfx/drive_c/users/steamuser/AppData/Roaming/bakkesmod/bakkesmod/bakkesmod.log`;
the plugin's lines start with `[class ReplayLab]`. `video/tourner.py` shows how to drive the plugin over RCON
(websocket on port 9002, password from `cfg/config.cfg`).

## Conventions

- Comments are short English sentences that say **why**, in the voice of the existing code; one comment block above
  each function. In-game and console strings are French **without accents** (the game's font has none; `Ascii()`
  converts players' names).
- Every console variable is registered once in `onLoad` and read through `opt` (`BindSettings`); write it with
  `cvarManager->getCvar(name).setValue(...)`.
- Shared state is touched under `mtx`. The ImGui window never calls game functions directly: it queues them with
  `gameWrapper->Execute`.
- Never create or destroy actors (bots, balls) in the middle of a physics step if it can wait for `OnFrame`.
- The blob only grows: a new version appends fields and `Decode` keeps reading the older ones.

## Pitfalls already met (see the comments in the code)

- Removing a bot from freeplay (`RemovePlayer`) crashed the game: bots are reused, freeplay is reloaded when too many
  pile up.
- Several `SpawnBot` in one frame lose bots: one at a time, each waited for.
- The game paints a car only when it builds it: a bot's look is set during `SpawnCar` (`OnSpawnCar`), and an existing
  bot gets a new look by being demolished and rebuilt. Orange cars ignore `SetCarColor`.
- Reading a car's player name when its owner is gone crashed the game (`NameOf`).
- Under Proton, after an Alt+Tab the game can stop seeing the keyboard: the plugin reads F keys with `GetAsyncKeyState`.
- Freeplay gives both nets team 1; the goal explosion's colour comes from the net's team.
- Function arguments have no evaluation order (MSVC goes right to left): one read per statement in `Decode`.
