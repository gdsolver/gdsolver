# Watching a run

What the mod puts on screen while it solves or replays, and the keys that drive
it. For what the iteration map's marks *mean*, see [ITERMAP.md](ITERMAP.md); for
how the loop works, [ARCHITECTURE.md](ARCHITECTURE.md).

## The panel

A small panel appears on the screens a level is started from — the main-level wheel, and any
individual level's own page, so custom levels included. It switches between three modes:
**Normal** (the game as it is), **Replay** (replay a stored solution for the level), and
**Solve** (solve the level in-process). Level selection itself is the game's own, so any level
can be picked. The panel is on those screens and nowhere else: everywhere else it would just
sit on top of the menu underneath it.

**It runs fast, dark and silent until it has something to show.** Candidate replays mostly
die and each one at normal speed would cost the length of the song, so the screen goes to the
fast loop and stays there. When a candidate finally clears, the level restarts and the solution is
played through properly — at 1x, with the artwork and the music. That is the run worth watching,
and it is the only one you are shown. (`F5` overrides this in either direction.)

## The overlay

While a level is on screen the mod draws a column down the top-left: a badge naming the run,
what the solve is doing, and the keys with the current speed:

```
iter 3   solving the tail from GD's own state   72s
level  [########............]  41.2%   best x 11372 / 27985
search [##############......]  72.4%   tick 18096 / 25000   x 19300   states 16000
```

Two bars, because they answer different questions. **level** is how far into the level the run has
actually got — the deepest point a replay reached — so it only ever grows, and it is the same
yardstick as the game's own percentage. **search** is how much of the current search is left;
reaching its end is the search finishing, which is not the same event as reaching the end of the
level.

## The keys

| key | |
|---|---|
| `F1` | hide the overlay. Refused while solving, and it never hides the bot badge |
| `F2` | pause / resume — `F3` / `F4` step 1 / 10 substeps (held down they repeat) |
| `F5` | rendering on / off. Only in a solve: there the frames go back to the fast loop, which is most of the speed. In a plain replay it does nothing — a replay you cannot see is not a replay |
| `F6` | hitboxes: off / on / **only** (the game's own debug drawing; "only" hides the artwork so nothing is left but the geometry the physics uses) |
| `F7` | replay from the start — works after the replay has finished too |
| `F9` | quit to the level screen |
| `F10` | the iteration map: where the repair loop spent its rounds. On during a solve, off in a replay, and this flips whichever it currently is |
| `←` `→` | seek back / forward. A tap is five seconds; holding accelerates. While the game is stopped they step a frame instead. Refused during a solve: the level is the loop's own verification replay and nudging it would spoil the round |
| `↑` `↓` | spectating speed, one notch (0.25x … 16x). Physics stays at 1/240 either way, so the trajectory and the tick numbers do not change |

There are no keys that let a player do what the game does not allow: no warp, no noclip, no forced
mode, no infinite jump. The overlay is for watching a run, not for changing one.

## The seek bar

A **seek bar** sits in the bottom-right of every replay. Click or drag it to go somewhere; the
level fast-forwards there (restarting first if you asked to go backwards, since a forced-scroll
game has no reverse) and hands the speed back exactly where it found it. It works on the results
screen too, which is when you most want it. `←` / `→` do the same thing in five-second steps.

The level is blacked out while it fast-forwards, with a `▶▶▶` indicator: the picture is running at
hundreds of physics steps a frame and is not worth looking at. Rendering is *not* switched off for
this — resuming it is the path that restarts the level (and crashes on the way back), so the level
is painted over instead.

## The iteration map (`F10`)

"Cleared in 62 rounds" says nothing about *where* the rounds went, and they are never spread
evenly: a level is usually solved by the first plan for nine tenths of its length, and then the
loop grinds against two or three walls. The map draws that distribution back onto the level — live
while the solve is still running, and over a replay of the solution, where `F10` asks for it. It
marks every place a replay died, what the loop was doing when it did, where the *model* was wrong
(often hundreds of pixels from the death it caused), and the path each round flew.

What each mark means, and how to tell a wall the model gets wrong from a wall with no route
through it, is in [ITERMAP.md](ITERMAP.md).

The data is the loop's own: it writes `itermap_lv<N>.txt` next to the solution when it clears, and
also when it gives up — a run that did *not* clear is the one whose map is worth reading. Runs made
before this existed can be recovered from their logs, which hold the same lines:

```bash
python py/itermap_from_log.py --all
```

```bash
python py/watch_plan_replay.py --level 22 --itermap
```

## Sound

A solve makes no sound while it is solving: it runs hundreds of attempts a minute with the physics
tens of times faster than the song, so every note lands in the wrong place. The sound comes back
with the picture, for the showing of the solution and for any plain replay, and the song is then
kept with what is on screen — pausing the game pauses it, and the spectating notch retunes it up
to 4x (past that it is paused and re-seeked on the way back down).
