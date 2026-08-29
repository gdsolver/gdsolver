# GD Solver

Give it a level and it finds an input sequence that clears it, by playing the
level inside your own copy of the game.

It does not re-implement Geometry Dash's physics. An approximate model drives a
**reachability search** over the 240 Hz physics ticks, and the game itself is
the verifier that decides whether a candidate plan really clears. Wherever the
replay disagrees with the model, the solver takes the state the game actually
had and carries on from there.

## Using it

A small panel appears on the screens a level is started from — the main-level
wheel and any individual level's own page. It switches between three modes:

* **Normal** — the game as it is. Nothing is touched.
* **Replay** — replay a solution already stored for this level.
* **Solve** — solve the level here and now.

Level selection is the game's own, so any level can be picked.

A solve runs **fast, dark and silent**. Candidate attempts mostly die, and each
one at normal speed would cost the length of the song, so the frames go to a
headless loop instead. When a candidate finally clears, the level restarts and
the solution is played through properly — at 1x, with the artwork and the
music. That is the run worth watching, and it is the only one you are shown.

While a level is on screen there is an overlay down the top-left: which round
the solve is on, how far the best plan has reached, and how much of the current
search is left. `F1` hides it, `F2` pauses, `F5` turns rendering back on, `F6`
draws hitboxes, `F10` draws the iteration map, and the arrow keys seek.

There are no keys that let you do what the game does not allow: no warp, no
noclip, no forced gamemode, no infinite jump. The overlay is for watching a run,
not for changing one.

## What it can solve

**All 22 official levels**, from cold — no stored solutions, no hints, nothing
but the level. That is also the supported set.

Custom levels are **not supported yet**. Nothing in the design is specific to
the official levels and many custom ones already solve as they are, but the
physics model has only been measured against what the official levels use, so a
level leaning on something it has never met is a level it is wrong about.

## Nothing is recorded while it drives

This is a TAS and research tool, not a way to fake records.

While the bot is driving — solving or replaying — **achievements, statistics,
coins and the level's own record are all blocked**, including completion
percentage, normal and practice best, attempts, jumps, orbs, diamonds and the
secret key. Every session ends by printing what it blocked and a
`level record changed:` line, which should read `none`.

**Playing it yourself records normally.** The block is tied to an automated
session being open, not to the mod being installed, so with the mod loaded but
idle your own attempts and progress count exactly as usual.

Replays driven by the mod are marked as bot input.

## Source

Fully open source, MIT licensed, with the measurements and the architecture
written up alongside it:
[github.com/gdsolver/gdsolver](https://github.com/gdsolver/gdsolver)
