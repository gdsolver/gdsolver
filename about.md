# GD Solver

Give it a level and it looks for an input sequence that clears it, by playing
the level inside your own copy of the game.

It does not re-implement Geometry Dash's physics. An approximate model drives a
**reachability search** over the 240 Hz physics ticks, and the game itself is
the verifier that decides whether a candidate plan really clears. Wherever the
replay disagrees with the model, the solver takes the state the game actually
had and carries on from there.

## Using it

Press a level's play button — on the main-level wheel or on any individual
level's own page — and a menu asks how to play it:

* **Normal** — the game as it is. Nothing is touched.
* **Replay** — replay a solution already stored for this level.
* **Solve** — solve the level here and now.

Under Replay and Solve a **Coins** switch makes the goal every coin as well as
the end. Under Solve, **Options** (top right) holds the settings about how the
level is searched, each with an (i) button that says what its values mean:
Section search (on by default), Avoid glitches (Off, Glitches only, or the
experimental Glitches + wave), and the search itself — the reachability DP, or
one of two experimental ones. Level selection is the game's own, so any level
can be picked. (Options can also make a click just play the level; holding the
play button then opens the menu.)

When a level has a Random or Advanced Random trigger, or an area or enter effect
with a random variance, the menu says so. While the bot drives, the game's random seeds are
set to the same values on every attempt, so a solution is found for, and
replays, one fixed draw; playing yourself is untouched.

A solve runs **fast, dark and silent**. Candidate attempts mostly die, and each
one at normal speed would cost the length of the song, so the frames go to a
headless loop instead. When a candidate finally clears, the level restarts and
the solution is played through properly — at 1x, with the artwork and the
music. That is the run worth watching, and it is the only one you are shown.

While a level is on screen there is an overlay down the top-left: which round
the solve is on, how far the best plan has reached, and how much of the current
search is left. `F1` hides it, `F2` pauses, `F5` turns rendering back on, `F6`
draws hitboxes, `F10` draws the iteration map, and the arrow keys seek. While a
solve runs with the screen dark, the replay panel (`Tab`) plays the game's own
record of the deepest attempt it has verified, in the free area to the right.

There are no keys that let you do what the game does not allow: no warp, no
noclip, no forced gamemode, no infinite jump. The overlay is for watching a run,
not for changing one.

## What it can solve

**All 22 official levels**, from cold — no stored solutions, no hints, nothing
but the level — with every coin or without, and the 16 levels of Meltdown, World
and SubZero and The Challenge, the six of them that have coins with every coin as
well. That is also the supported set. (Those 17 are not in the main game's level
list: the solver reads them from level files, which are not distributed with it.)

Custom levels can be solved too, without a guarantee. Nothing in the design is
specific to the official levels: of 100 rated custom levels drawn at random, 93
cleared within 20 minutes each, and some of the newest gimmicks do not solve yet. The
physics model is measured against what the official levels, and the calibration
levels that come with the source, use, so a level leaning on something it has
never met is a level it is wrong about. The play menu says so before a solve: a
yellow line when a custom level uses objects from 2.0 on, a red one when a level
holds something the model cannot express at all. Where the model has no answer,
the section solver searches the game itself, and a plan the game clears is a
solution however it was found. Platformer levels are refused: the search has no
steering input to choose.

This version is for GD 2.2091 on Windows with Geode 6.0.0 or newer, and for the Steam
executable it was built for: on another one it stays off and says so on screen. There is
no Android build of this version; it follows the Windows release.

## Nothing is recorded while it drives

This is a TAS and research tool, not a way to fake records.

While the bot is driving — solving or replaying — **achievements, statistics,
coins and the level's own record are all blocked**, including completion
percentage, normal and practice best, attempts, jumps, orbs, diamonds and the
secret key, and on 2.209 the upload of where you die, the play time and the
death sessions. Every session ends by printing what it blocked and a
`level record changed:` line, which should read `none`.

**Playing it yourself records normally.** The block is tied to an automated
session being open, not to the mod being installed, so with the mod loaded but
idle your own attempts and progress count exactly as usual.

Replays driven by the mod are marked as bot input.

## Source

Fully open source, MIT licensed, with the measurements and the architecture
written up alongside it:
[github.com/gdsolver/gdsolver](https://github.com/gdsolver/gdsolver)
