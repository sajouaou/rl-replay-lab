# Report: Replay Lab 4.0 (what I did, found, corrected, and what I still need)

Everything below was done without the game (BakkesMod was not injected; I did not touch your game session). Only the
plugin was changed (`bakkesmod/`), not the web lab. The code compiles with no warning, the replay reader and the new
playback timeline were checked offline on all 86 replays of your `Demos` folder. Agents: one researched the game's
internals (BakkesMod plugins, decompiled game scripts), reviews were run on the code; the documentation and the first
review agents were stopped by the 5-hour usage limit, so I wrote the docs and did the refactor myself. What needs the
game is listed in `NOTES.md`.

## What I found (why playback did not look like a real match)

I wrote `bakkesmod/rlab_replay/examples/flow.rs`, which prints how a replay plays its match frame by frame, and ran it
on your replays. What a replay file really contains around a goal:

1. **The goal frame**: the game state becomes `PostGoalScored`, the ball goes to sleep where it exploded, and the
   explosion record names the scorer.
2. **About 3 s of play**: the cars keep driving (the post-goal).
3. **A hole in time** of 0.5 to 9 s: the in-game goal replay is not recorded (365 of the 520 holes are longer than
   3 s, always after a goal).
4. **The kickoff**: all cars and **a new ball** are created at their kickoff spots in one frame, then the countdown
   number goes 3, 2, 1 about one second apart, then `Active` (GO).

Demolitions: the car stays in the data, hidden and asleep where it blew up, for about 85 frames, then disappears and
comes back at a spawn point 3.05 s later (all 270 demolitions follow this).

So the old plugin:
- played through the holes: for up to 9 s the cars slid along their last velocity and back;
- took the kickoff reset (all cars missing for one frame) for **demolitions**: every car exploded and was missing for
  part of the countdown, then respawned at a random spot and was moved to its place;
- missed real demolitions: the car stayed visible and frozen for 3 s, then exploded at the moment it should respawn;
- left the ball asleep in the net (no explosion) and showed no countdown;
- started the score at 0-0 even in a match joined late (one of your replays starts at 3-0, another at 1-1).

## What I did

**Replay reader (Rust, blob version 5)**: the phase of each frame (countdown, playing, goal) and the kickoff countdown
number, the score the replay starts from, each goal's scorer and assist, the demolitions (the demolished car marked
absent), each player's throttle and steering (wheels) and powerslide, and the in-game name of each replay for the list.

**Playback like the real match** (`ReplayLab/Match.cpp`):
- kickoff: **the game's own countdown** (3, 2, 1, GO! with its sounds, as freeplay kickoff plugins do it), sent at
  the replay's exact moments; the cars and ball appear directly at their spots;
- goal: the ball **explodes with the scorer's own goal explosion** (the game's `Ball.Explode` with the scorer bot's
  player info; the bot already wears his loadout, explosion included), a banner in his team's colours (scorer, assist,
  ball speed in km/h), the ball dissolves like in the game, the cars play the 3 s, then **the goal's replay** fills the
  hole (6 s before to 2.5 s after the goal; black bars, red dot and REPLAY bottom left, scorer / assist / km/h,
  camera on the scorer with ball cam), the screen fades through black, a new ball for the kickoff (the game also makes
  a new ball at each kickoff);
- demolitions at the exact recorded moment;
- the right score from the start, the end-of-match banner;
- wheels that steer and spin like the player's, powerslides.

**Simulation**:
- simulating during a kickoff or after a goal starts at the next kickoff **with a real countdown**: everyone frozen
  on their spots until GO (also an option to start any simulation with 3-2-1);
- a simulated goal explodes with the explosion of the last player of that team who touched the ball, shows the banner,
  and is replayed 2.5 s later (option to turn that off for quick training, e.g. practising a save).

**Branches** (exploring "what if"s, for videos): every simulation is kept (8 at most) and can be watched again like a
replay with all cameras, slow motion and cinema mode, and simulated from again (a branch of a branch). F11 / reset
always retries the last simulation from what it started from.

**Controller**: long presses on the d-pad (0.6 s) give a second action: back to the original replay, cinema mode, slow
motion, next branch. Throttle/brake while watching = fast forward/rewind; A skips a goal replay.

**Memory**: branches recorded at 60 Hz instead of 120 (half the memory) and capped; per-tick allocations removed;
painted-car bookkeeping pruned; since freeplay crashes when a bot is removed, bots are reused and freeplay is reloaded
when more than 5 extra bots have piled up (that removes them all).

**Readability and docs**: the 2,900-line `ReplayLab.cpp` was split by topic with a script that moves each function's
text unchanged (checked line by line): `Input.cpp`, `ReplayFile.cpp`, `Scene.cpp`, `Playback.cpp`, `Simulation.cpp`,
`Match.cpp`, `Branches.cpp` (was `Scenario.cpp`), `Camera.cpp`, `Hud.cpp`, `Window.cpp`, shared `Rules.h` and `Draw.h`;
the header groups every member under the file that uses it. Settings are bound once (`Settings opt`) instead of looked
up by name (about 50 lookups, several per tick); background threads are joined (no plugin code running after unload);
a dead variable removed. Documentation for people and AI:
`docs/OVERVIEW.md` (concepts), `docs/ARCHITECTURE.md` (technical), `docs/DEVELOPING.md` (build, debug, conventions),
`CLAUDE.md` (briefing for AI assistants). The player README (`bakkesmod/README.md`, French) describes every feature.

## What I corrected

- Kickoff resets no longer demolish every car; cars appear at their spots under the fade.
- Real demolitions happen at the right moment instead of 3 s late.
- No more 0.5-9 s of drifting cars after each goal.
- The score of a match joined late.
- A stale controller input could keep moving the free camera (now ignored after 0.15 s).
- Goal phases with a short lag gap before the real hole (5 of your replays) would have shown a goal's replay twice or
  in the middle of the post-goal; goals whose replay the players skipped (no hole at all, 4 replays) would have had
  no replay. Now each goal phase has exactly one cut, at its largest gap, found when the replay loads.
- The explosion's colour comes from the net's team, and freeplay gives both nets team 1: the net gets the defending
  team for the explosion and its own back the next frame.
- New balls are made between frames, never inside a physics step.
- From the final code review (all fixed): no countdown over a goal's replay, and a goal's replay never reaches back
  into the previous goal; aborted simulations under 0.5 s no longer block the branch cycle or push good branches out;
  F11 / F4 can no longer be hijacked by a goal replay due at the same time; a seek or "Voir" stops a goal's replay;
  no goal replay is invented after the match's last goal; a replay just read is picked up by the game's next frame
  instead of being queued to the game (nothing of the plugin can run after it is unloaded); the nets' teams are put
  back on unload; a simulation started after the last goal is not treated as a kickoff; a kickoff's countdown can no
  longer start with the previous kickoff's "1".

Still open (lower confidence, to watch in the game): the window holds the plugin's lock while queuing actions to the
game (a deadlock is possible only if BakkesMod holds its own queue lock while running them; it never happened so far);
if the game state says "goal" a frame later than the goal explosion, the exploding ball could be replaced at once.
- (The review agent's findings and their fixes are listed below.)

## What I need from you

1. **Test the six checks in `NOTES.md` part 1** (explosion, goal replay, kickoff, demolition, simulated goal, kickoff
   countdown) and send me the BakkesMod log if anything is off. The built DLLs are in `bakkesmod/build/`;
   `./install_plugin.sh` copies them (I did not install them in your game). The goal explosion uses a game function no plugin of
   yours used before; if it misbehaves, `rlab_explosion 0` turns it off and everything else still works.
2. Your opinion on the look: countdown size, banner, REPLAY tag, fade length, goal replay length (6 s before the goal,
   1.5 s after).
3. Whether you want branches **saved to disk** (to film them another day), and named.
4. Whether the presentation video should keep the old timings (see `NOTES.md` part 4).
5. Should a simulated goal be followed by a **real kickoff** (bots and you back on the kickoff spots, countdown, play
   on: the branch becomes a whole alternative match)? It is possible but a bigger change; I did not start it.
