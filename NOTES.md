# Notes: what to test, and the feedback I need (Replay Lab 4.0)

Written while you were away. Nothing here could be tried in the game (BakkesMod was not injected and I did not want
to touch your game session), so these are the checks that matter most. Everything compiles and the replay reader was
checked offline on all 86 replays in your `Demos` folder.

Install first: `cd bakkesmod && ./install_plugin.sh`, then in the game's BakkesMod console (F6):
`plugin unload replaylab` and `plugin load replaylab` (or restart the game). The plugin says "Replay Lab 4.0 charge"
in the console.

If something goes wrong, the BakkesMod log is
`…/compatdata/252950/pfx/drive_c/users/steamuser/AppData/Roaming/bakkesmod/bakkesmod/bakkesmod.log`; every line of
the plugin starts with `[class ReplayLab]`. Sending me that log (or the end of it) is the most useful feedback.

## 1. The risky parts first (they use game functions never used before)

| # | Do this | Expected | If it fails |
| --- | --- | --- | --- |
| 1 | Open a replay (F3), play it until the first goal | The ball **explodes with the scorer's own goal explosion**, in his team's colour | Tell me what you saw (default explosion? no explosion? crash?). `rlab_explosion 0` turns explosions off |
| 2 | Keep watching after that goal | The ball dissolves, cars play 3 s, screen goes dark, **REPLAY** of the goal (black bars, red dot + REPLAY bottom left, scorer / assist / km/h at the bottom, camera on the scorer), dark again, kickoff with **the game's own 3, 2, 1, GO! and its sounds** | Does a new ball appear at the kickoff? Ever a second ball, or none? Did the countdown show and beep? |
| 3 | Same replay, a goal where you are the scorer and you drive yourself ("Toi") | Your own goal explosion | |
| 4 | Watch a demolition in a replay | The car explodes at that moment and comes back ~3 s later, at the same time as in the real replay | Car frozen, invisible too long, or exploding twice? |
| 5 | Simulate (F5) a few seconds before a real goal, let it go in | The ball explodes with the explosion of the last player of that team who touched it; 2.5 s later the goal is replayed | Wrong explosion? The log says `le ballon explose (... explosion de NAME)` |
| 6 | Press F5 during a kickoff countdown | Cars on their spots, frozen, the game's **3, 2, 1, GO!**, then everyone moves at once | Can anyone move before GO? Did the numbers show? (`rlab_tableau 1` is needed) |

## 2. The rest

- Replay list (F3): replays you named in the game show their name first, in quotes.
- A replay where you joined a match already started: the score starts right (e.g. 3-0), not 0-0.
- Wheels: watching a player, his front wheels turn when he steers and the car drifts when he powerslides.
- Tries and branches: retry as often as you want (d-pad down); nothing is kept until you save (d-pad down held 0.6 s);
  the F3 window lists "Dernier essai ... pas sauvegarde" with "Sauvegarder", then "Branches sauvegardees"; F5 inside a
  branch simulates from it (and that try can be saved as "Branche N depuis ... de la Branche M"); d-pad up held cycles
  original -> last saved branch -> ... -> original.
- Controller: d-pad down = retry, held = save; Back = simulate, held = next role of the player shown; d-pad up held =
  next branch; d-pad left held = cinema mode; d-pad right held = slow motion x1 / x0.5 / x0.25; while watching, throttle
  = fast forward, brake = rewind; A skips a goal replay.
- Quick training: untick "Simulation: un but revu juste apres" (or `rlab_revoir_simulation 0`); after a goal F11 /
  your reset button restarts at once.
- Start modes (F3 options): "tout de suite", "quand tu bouges ta manette", "compte a rebours 3-2-1".
- Memory: open several replays in a row (different players and cars). When more than 5 extra bots have piled up, the
  next replay reloads freeplay (a loading screen), which removes them.

## 2b. Latest changes (to check first)

| Do this | Expected |
| --- | --- |
| Put "Toi" on a player, simulate (F5) | You drive his own car (his body, hitbox and look) with your controller, the camera behind it, no car blown up; his boost bottom right. Does it feel like driving (camera, ball cam with your button or F8, latency)? |
| During a simulation take some boost pads, then restart (d-pad down / F11) | All pads back, except the ones taken in the replay at that moment; those come back when they did in the replay |
| Watch a replay, pause it near pads that were just taken | The pads stay taken while paused; rewinding and seeking puts them as in the replay |

| Simulate a kickoff, take boost, restart (d-pad down) | Every pad you took is back |
| A kickoff or a 50/50 against a ghost | Real collision: he is pushed like a car, then plays as a Psyonix bot ("touche, bot" on his name) |
| Your ball cam button while driving his car | Ball cam on / off as in a match |

`rlab_voiture_joueur 0` goes back to driving your own car.

## 3. Feedback I would like

1. For each check of part 1: OK / not OK, and what you saw. Screenshots help (`rlab_photo` saves one).
2. Does it now **look like the real replay**? What still feels different (timing of the goal replay, fade length,
   size or place of the countdown, the goal banner, the REPLAY tag, the end of match banner)?
3. Is the **controller mapping** clear and comfortable? Any button you would rather use (e.g. for slow motion)?
4. For video work: what is missing to film branches (camera paths? saving branches to disk to film them another day?
   naming branches?).
5. Kickoff simulation: does the countdown make kickoffs feel like a real match? Should the bots also get the real
   kickoff positions when you simulate from somewhere else?
6. Performance: any stutter when goals explode, at kickoffs, or after many simulations?

## 4. For the presentation video (`bakkesmod/video/tourner.py`)

Its scenes now get the new behaviour: the simulated goal in "vrai but" explodes, and 2.5 s later it is replayed (the
plugin switches to watching the branch). To keep the old scene timing, send `rlab_revoir_simulation 0` at the start
of the script (and `rlab_revoir_buts 0` if a scene plays through a real goal and must go straight to the kickoff).
