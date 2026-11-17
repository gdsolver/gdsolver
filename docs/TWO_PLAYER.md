# Two-player levels

A level with Two-Player Mode on (header key `kA10`) gives the second player a button of its own. This page is what the project has measured about
it, what the solver does with it today, and what it does not do yet. Everything here was measured on GD 2.209 (the 22091 executable) by replaying
plans in the game and reading the per-tick dump; the calibration rigs in `data/rigs/calib_p2*.lvl` are the small levels behind the table.

## What the solver does with one

* **The plan carries both buttons.** `input=<tick>,<mask>`: bit 0 is player 1's jump, bit 1 player 2's. The mod presses player 2's with the game's own
  call for the second player (`handleButton(down, 1, false)`). In a level that is not two-player the game would send that press to player 1, so there
  a bit-1 edge is ignored (and said once in `result.txt`). A package without this input path reads a cfg `input=` line with bit 1 as a release, so the
  harness (`py/gdtas`) will not launch such a plan on a package that lacks it, and its one-button readers (`plan.read_inputs`) refuse a plan with bit 1;
  `plan.read_input_masks` keeps both buttons.
* **The model (`dp`) knows the level is two-player** from `twoPlayer=1` in `levelsettings.txt`, which the mod writes only for such a level. Then the
  second body takes its own button (`stepBoth`'s `input2`) and no gravity portal that one body crosses flips the other; `leveldp --replay` reads the
  mask. A level that starts as a dual (`kA8=1`) has both bodies from tick 1, on a band the level's header sets, which the model cannot know: the loop
  takes one tick of the game before the first solve (a two-player level does so even when nothing in it moves) and starts the solve from that row, as a ladder
  anchor starts a tail from the game's row.
* **The model's rules for the second body (on by default, each with a `--no-<name>` off arm).** A dual portal of a two-player level gives the new body
  the first body's inverse gravity and negated vy (`--dualbornlinked`); a body born by a dual portal holds nothing until the plan releases and presses
  its button (`--p2born`), and an anchor's dead hold reaches it (`--holdseed2`); its wave reads its own previous button (`--p2held`); a search with no
  second body at its anchor writes the first body's levels for both buttons (`--p2shared`); the pass in which its solo portal ends the dual reaches no
  pad or ring after that portal (`--p2soloafter`); both bodies of a dual press their gravity rings on the tick the press begins, in the one case measured (two turned blue rings; `--dualorblate`); and a cube on the
  dual band's ceiling takes a gravity portal as the game does (`--dualbandportal`, below).
* **The search plans each body against the other's plan, one after the other.** A search call on such a level searches the first body against a ghost that
  holds nothing (a second body stepped with its plan, whose death ends nothing), then the second body, swapped into the first slot, against the first's plan
  (the first body's per-tick buttons are the ghost's levels), and merges the two into one mask plan; the outcome is the worse of the two. This is not a
  joint search: two bodies' reachable sets multiply (about 50 each on a cube stretch, 10^3 to 10^5 on the rest of the level it was measured on), and the two
  share nothing but the clock where a level has no triggers (the speed portals count). The game verifies the merged plan as it does any, and the ladder
  re-anchors on the game's row (with the second body's x offset and button) where it dies. What this cannot do: a level where one body's route changes the
  other's (a trigger that only one body can reach and that opens the other's way) -- the ghost holds nothing, so the other body is solved against a world the
  first has not changed.
* **The section solver never runs on one.** It plans one body's button against a world it cannot give a second body, so a two-player level is not offered to it
  (the cfg and the autorun commands are refused for the level, and the loop's own firing is skipped); the loop alone plans the level.
* **A fixup names the second body's button.** The loop records where the model and the game part ways as a fixup keyed on the step's input. On a
  two-player level the record also carries `in2` (the second body's button), and a fixup is applied only to a step made with that button; a record without
  `in2` (any level that is not two-player, and old files) matches either, as before.
* **Three flags for a pair that has run apart (all off by default; cfg `dppin` passes the first two).** A custom two-player level starts with a Teleport
  trigger (3022) that a Spawn loop fires again on 5 of every 6 ticks, onto a target a Move trigger carries, until a Stop halts the loop: player 1 stands at
  x 644.5 for 165 ticks (y 2085 -> 2055, vy reading 0) while player 2 runs on, 550 px behind. The model fires a teleport trigger once, at its crossing.
  `--pinrows <file>` (lines `tick,x,y`) puts player 1 at the recorded spot at the end of those ticks (the loop writes them from its own recording: a tick
  whose x equals the previous or the next tick's, in a run of three or more); in a swapped pass that is the second slot. `--p2secx` reads the second body's
  section cell (the follow-copies pass, a level with dynamicLevelHeight) at its own x, `K.x + dx2`, not the group's: player 2's gravity portal 550 px away was
  "far" and sent off. With both, and `--startp2dx`, the model follows both bodies to 0.0000 for the 376 ticks until player 1 meets a wall, where the game kills it.
  `--p2taps`: a Tap trigger (1595) of a two-player level is fired by the second player's press edge and armed by the first player's x (a mask-1 press at the same
  tick leaves the door the Tap opens shut; mask 2 opens it three ticks later). The model does not know what the Tap opens: its chain is not in the level data
  the exporter writes (nothing targets the Spawn triggers that start it).
* **A Tap the second player has to press: `--p2tappress` (off; cfg `dptap`).** A search that holds the other body on nothing cannot ask for the one press that opens
  a door for the first player. With the flag the per-body wrapper presses the second player's button, once, `--mustpress t:t'` in the second body's pass, on the input
  grid, three ticks after the first body's plan passes the Tap's x (the first call's pin rows come from the no-death pass's own recording). The model does not know
  what the Tap opens, so the press is blind; the game's recording of the attempt then shows the door open to every later call. A Tap in toggle mode takes every press
  (measured: a second press at t=300 starts the door down at 309), so `--p2holdx X` (cfg `dptap` passes the farthest wall's x + 100) makes the wrapper keep the second
  body off the button (`--mustrelease`) until the first body's plan is past X. Measured: with the tap alone the first player gets past its first wall in the game (first death
  654 -> 750) and dies at the second (the second player's own presses had shut the door again). Not yet
  closed: the loop only runs plans that look deeper, so the plan with the hold is not run while the recording still has the door shut.
* **A button that moves the world: cfg `dpswitch` (off; dp `--p2switch --switchtab`).** Seen on a custom two-player level: every press of the second player flips the
  same set of walls and hazards, at any time in the level, and the first player's presses move nothing. After the bootstrap pass the mod runs the level twice more
  (nodeath, one body pressing every 600 ticks) and sets each recording against the baseline: the objects that differ within 12 ticks of a press are what it
  switches, their rows after it are what it did, and the body whose presses moved more gets the table (`dp_switch.txt`; format in `dp/src/dp/switch_model.hpp`).
  The table is a relation, not a parity: a press plays the object's first curve when it is the odd one in the schedule, the second when it is even, as differences
  onto the object's own timeline, and the geometry for any schedule of presses is that table evaluated and written as the last `--groups` layer (so it wins over the
  recordings, which have the walls where their own attempts' presses put them). The per-body wrapper searches the first body, looks at the wall of the table that
  stands in its way where it stops, adds the latest press that has finished its curve by then, and searches again; the second body's pass is then forced to press at
  exactly those ticks and nowhere else (`--mustpress`, `--mustrelease`), over the same geometry. A walk of a plan (`--replay`) takes its schedule from the plan's own
  presses, and an anchored call takes the presses before its start from `--switchpre`. Measured: from the first wall (t=654) the first body gets to t=4405
  with five presses; before it stopped at x=1272.
  The role of a body is read per level by the nodeath/press scans above, not assumed; the table is for one body and for presses at any time (a Tap armed late
  would need probes after its arming). Presses are placed on the input grid; only walls the first body meets within a few pixels of its stop are considered.

* **Which body can walk its side with no input: cfg `nodeath` takes a body.** `nodeath=1` swallows every body's death (as before), `2` only player 1's, `3` only
  player 2's; with any of them a replay session also writes each swallowed call's `killer:` line (who, x, y), so an idle run lists where each body would
  have died (a body that stays inside a solid reports every tick, so after its first stuck stretch the list says nothing more about it). `nodeath=2` with no
  input and a level whose player 2 needs no jump runs to the end. Whether a press changes the world is read from two grouptrace runs of the same level
  (`nodeath=1`, one press of one body every 600 ticks against none): the objects whose samples start to change within ten ticks of a press are what that press
  switches.

## What is supported

Two-player levels are supported, as an experimental feature, where each player drives their own body: both bodies run, or one only follows. A level where one body's press is used as a trigger at any moment
(flipping walls or doors, a "switch" role) is not solved yet: the parity and the timing of several presses have to come out right together, and the search does not get that far
(the model itself agrees with the game on such a level). cfg `dpswitch` and dp `--p2switch` stay as the tool for that attempt, off by default.

## What was measured

| rig | question | answer |
|---|---|---|
| `p2mask` | which body jumps on which mask | mask 1 moves only player 1, mask 2 only player 2 (the flipped body jumps toward the floor), mask 3 both |
| `p2mask_solo` | `kA10` without a dual start: is the second player there | no: one player object, the second player's columns stay empty |
| `p2grav` | does a gravity portal flip only the body that touched it | yes, in a two-player level: the model with `twoPlayer=1` (no link) follows the game to 0.0000 on both bodies for 4,931 ticks of this rig |
| `p2bandgrav`, `p2bandgrav_dual` | a body seated on the dual band's own (invisible) ceiling takes a gravity portal | on that tick it takes no gravity step: y and vy stay (345.000 / 0.000) and it falls from the next tick, whichever body it is, in a two-player level and in a plain dual (`kA10=0`, where the portal also flips the other body, the link a dual has). A cube seated on an object takes the old gravity's step, halved (levels 5, 6, 7). Modelled by `--dualbandportal` (on by default) |

On a two-player level of the fetched set (not named here) the model, started from the game's own tick-3 row, follows both bodies to 0.0000 over the
first 478 ticks, with no input and with a plan pressing player 1, player 2 and both; the game ends the attempt there on the second body's death.

## Not measured

`p2hold` (does the second body's cube jump repeat while its button is held), `p2orb` (whose press fires which orb, and whether the other body's ring
is spent) and `p2death` (whether one body's death always ends the attempt) are built but have not been read. The band's floor above the ground layer
with a gravity portal is not covered by `--dualbandportal`.

## Reproducing a row

```
python py/mklevel.py p2bandgrav --out data/rigs/calib_p2bandgrav.lvl      # writes the .units.json and .plan.txt beside it
```

Replay the plan in a worker (`levelfile=` the .lvl, the plan's `input=` lines in the cfg) with `dump=1`, then run `leveldp --replay` on the same plan
from the game's row (`quick_regress.start_fields` builds the `--start` argument) with `twoPlayer=1` in the level settings, and compare the two bodies' y.
