# Watching a run

What the mod puts on screen while it solves or replays, and the keys that drive
it. For what the iteration map's marks *mean*, see [ITERMAP.md](ITERMAP.md); for
how the loop works, [ARCHITECTURE.md](ARCHITECTURE.md).

## The play menu

Pressing a level's play button — on the main-level wheel, or on any individual level's own
page, so custom levels included — opens a menu before the level starts. It offers three modes:
**Normal** (the game as it is), **Replay** (replay a stored solution for the level), and
**Solve** (solve the level in-process), and under Replay and Solve a **Coins** switch, greyed out
on a level that has no coins. Replay is refused on the spot, with the reason, when the level has
no stored solution of that kind. **Start** (or `Enter`) starts the level in the chosen mode,
`1` / `2` / `3` pick a mode, and closing the menu starts nothing. Level selection itself is the
game's own, so any level can be picked. A platformer level gets no menu: the solver does not play
platformer mode, so its play button just plays it.

Under Replay, Options has **Fixed replay step** (off by default): the replay is advanced in fixed
steps of 1/60 s, so it no longer depends on the length of the frames; on a screen faster than 60 Hz
the motion then looks like 60 fps. Off, the replay runs on the game's own frame time.

Under Solve an **Options** button in the top-right corner opens the settings about how the level is
searched, which keep the last Start's choices. Each has an (i) button that says what every value of it means; the menu itself shows
only one line (yellow, or red for an experimental search) naming what is not at its default, with an (i) button of its own:

* **Section search** — ticked by default. When a wall is one the model cannot get past, the solver
  hands it to a search of the game itself over that stretch. Unticked, the solver never does, so a
  level that needs it is not solved; while it is unticked the menu's Options line says so. (The
  cfg key `dpsecauto=0` is the same switch.)
* **Avoid glitches** (experimental), picked with the arrows (or `↑` / `↓`): **Off** (the default);
  **Glitches only**, where the searches drop the routes that rest on frame-level glitches; or
  **Glitches + wave**, which adds the wave's behaviour to that: the wave's button is not planned to
  change faster than a hand can (a press or a release is kept for at least a set number of ticks),
  and the wave keeps a clearance from the floor, the ceiling and solids instead of crawling along
  them. Among the routes that remain it prefers the one with fewer button changes and the one that
  flies nearer the middle of its corridor (a preference, it drops nothing). Either restriction narrows the search on purpose, so it can lose the only route there is, and while it
  is chosen the menu's Options line says so. Some levels cannot be passed without a hairline dodge, so where the solver finds no route it eases
  Avoid glitches in steps, for that stretch only: the wave's behaviour first, then the margins halved,
  quartered and off. About 900 px past the wall it takes one step back, and the next 900 px another,
  until it is as chosen again. Each step is said on screen and in the result file. A choice saved from the old checkbox reads as Glitches only (ticked) or Off.
* **Search** — which search answers the solver's calls, picked with the arrows (or `←` / `→`):
  **DP (default)**, the reachability DP every result in this repository comes from; **Beam**,
  which keeps the best states by the solver's own score at each tick; or **Monte Carlo**, random
  playouts from a fixed seed. The last two are experiments, more likely to fail a level the DP
  clears, and while one is chosen the menu's Options line turns red. A call the chosen search cannot
  follow is answered by the DP, and the log says so on a `search:` line; Monte Carlo cannot follow a
  coin route, so with Coins on the DP answers every call of the solve and the result file says that
  once as a warning. Whatever the search, every plan is flown and judged by the game.

The choice is made per play. The menu opens on Replay when a solution for the level is stored (with Coins ticked when the coin solution is), and on Solve when none is. Turning the mod setting **Choose
the mode on Play** off makes a click on the play button just play the level, as without the mod; holding
the button opens the menu instead (a ring fills around it while you hold, and goes if you let go early), so the
setting can always be turned back from the menu's Options. Solve faster (not reproducible) is no longer a setting;
the cfg key `dpplainbeside=3` still lets the plain search start beside the cap ladder and be taken as soon as it will do:
usually faster, but the same level can take a different route and time from one solve to the next. Every plan is still
flown and judged by the game.

Under Replay and Solve, up to three lines about the level itself sit above the bot note. None of
them refuses the run.

* **Red — not modelled by the solver:** the level holds something the model cannot express at
  all, so it plans blind there and only the section solver can get through: such a level is
  unlikely to be solved.
  The line names what it found: teleports with several exits, teleport
  orbs that push you, teleport triggers keeping an offset or set off by an item, a delayed spawn
  or an event no pad or orb raises; gravity triggers for player 2 (in a level with a second
  player) or set off by an item, a delayed spawn or an event no pad or orb raises; Follow Player Y triggers with a delay or set off by touch or a spawn, or more than two
  of them running at once; more gravity portals than the solver can hold (past 128 it shares their slots, which fails in a level that
  also reverses the player or puts two of them close together); a level that runs differently
  from one attempt to the next (an Item Compare or Item Edit that reads the attempt count, or an
  item Item Persistence keeps, sets off triggers that reach something the player touches or the
  player itself — a level that only shows the count is not named). The line gives each a few
  words ("some teleport triggers") and names two at most, then how many more; the game's log has
  them in full, on the `play menu: lv<N> checked:` line.
* **Yellow — a two-player level**: the second player has a button of its own, and the solver plans it,
  each body against the other's plan; the support is experimental, and a level where one player's press
  is a switch for the other's way is not solved yet ([TWO_PLAYER.md](TWO_PLAYER.md)).
* **Yellow — objects from 2.0 on** (custom levels only): the model is measured against the newer
  mechanics one at a time and much of that is still to do, so such a level is slower to solve and
  may not be solved. It counts the objects of types that did not exist before 2.0 (robot, spider,
  swing, teleports, the newer rings and pads, collision and force blocks, area effects) and the
  triggers that act on the game (move, rotate, toggle, spawn, item, camera, time and the like);
  1.8's dual and slopes and 1.9's wave, recoloured blocks, spikes and decoration from the newer
  versions, and triggers that only change colours, sound or shaders, do not count. The line names
  the newest version among them — "Its newest objects are from 2.2" — so a level with 2.2's
  objects reads differently from one whose newest are from 2.0. Each object is dated by the
  oldest version of a server level that holds it, so the date is a guide: an object can read newer
  than it is, never older. The version says how new the mechanics are, not how likely the solve is:
  on the v0.5.0 build, a hundred random levels cleared about as often whether their newest objects
  came from 2.0, 2.1 or 2.2.
* **Already solved:** with a solution stored for the Coins choice, red and yellow are left out —
  the level was solvable. Under Solve a yellow line says instead when that solution was filed by
  another version of the mod (or before versions were recorded), since solving it again with this
  one may not succeed. The version sits beside the solution, in `solution_lv<N>_dp.txt.ver`
  (`_coins.txt.ver` for a coin solution).
* **Blue — random numbers:** the level has a Random or Advanced Random trigger, or an area or
  enter effect with a random variance (a spawn delay's or an Advanced Follow's variance is not
  detected yet). While the bot drives, the game's random seeds are set to the same values before
  every attempt, so a solve and a replay meet the same draw every time; playing the level
  yourself is untouched (cfg `rngfix`, on by default).

**It runs fast, dark and silent until it has something to show.** Candidate replays mostly
die and each one at normal speed would cost the length of the song, so the screen goes to the
fast loop and stays there. When a candidate finally clears, the level restarts and the solution is
played through properly — at 1x, with the artwork and the music. That is the run worth watching,
and it is the only one you are shown. (`F5` overrides this in either direction, except while a
section solve runs: its search moves the world back and forth through geometry it does not put
back, so the screen stays off, and the replay that takes the run to the section head goes back to
the fast loop even if `F5` had put it at the spectating speed, until the solve ends and the screen
and the speed come back as they were — see [SECTION_SOLVE.md](SECTION_SOLVE.md).)

**Leaving a level ends its solve.** Quitting mid-solve (`F9` or the pause menu) stops the search
that level started on its next layer, and the next level you pick starts from a clean slate.

## The overlay

While a level is on screen the mod draws a column down the top-left: a badge naming the run,
what the solve is doing, two progress bars, and the keys with the current speed. The text of a
solve reads like this (the `coins` line only with Coins on, counting this attempt and the best any
attempt has had; while no search runs, the last line gives the player's x and the anchor instead):

```
iter 3   solving the tail from GD's own state   72s
coins 1/3 this attempt   best 2/3
level   41.2%   best x 11372 / 27985
search  72.4%   tick 18096 / 25000   x 19300   states 16000
```

Under it, the two bars are drawn as the game's own level bar, because they answer different
questions. The upper one, **level**, is how far into the level the run has actually got — the
deepest point a replay reached — so it only ever grows, and it is the same yardstick as the game's
own percentage. The lower one is whatever the solve is doing now that has a measurable end: the
search (in ticks; reaching its end is the search finishing, which is not the same event as reaching
the end of the level), a recording of the moving geometry, a section search (in layers), or the
game flying a plan. It is hidden while the level is held still.

## The keys

| key | |
|---|---|
| `F1` | hide / show the overlay. It does not hide it while solving (showing it always works, and a solve starts with it shown), and it never hides the bot badge |
| `F2` | pause / resume — `F3` / `F4` step 1 / 10 substeps (held down they repeat) |
| `F5` | rendering on / off. Only in a solve: there the frames go back to the fast loop, which is most of the speed. In a plain replay it does nothing — a replay you cannot see is not a replay |
| `F6` | hitboxes: off / on / **only** (the game's own debug drawing; "only" hides the artwork so nothing is left but the geometry the physics uses) |
| `F7` | replay from the start — works after the replay has finished too |
| `F8` | end a human takeover (a debugging session, [TAKEOVER.md](TAKEOVER.md)); does nothing elsewhere |
| `F9` | quit to the level screen |
| `F10` | the iteration map: where the repair loop spent its rounds. On during a solve, off in a replay, and this flips whichever it currently is |
| `Tab` | the replay panel: GD's own record of the deepest verified attempt, played in the free area from the middle of the screen to the top-right ([REPLAY_PANEL.md](REPLAY_PANEL.md)). On by default while a solve runs with the screen dark (never in the showing of the solution); the key goes round the whole path (ending with the model's plan in blue) -> its last stretch -> off. While it is on screen, `F2` `F3` `F4` and the arrows are the panel's (stop, step, seek, pace); with the screen on they are the game's. `replaypanel=0` in `autorun.cfg` starts it off |
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

A replay takes a checkpoint every 600 ticks (2.5 s) as it plays, and a seek starts from the latest
one at or before its target instead of from the start; the way up to that checkpoint also skips the
visibility pass and the draw. A run restored from a checkpoint is checked tick by tick against the
run from the start, and the first difference turns the checkpoints off for the rest of the session
(`seekcp: parted` in `result.txt`). `seekcp=0` in `autorun.cfg` turns them off, `seekblind=0` the
faster way up to them.

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

The replay opens on a free worker and leaves every other game running, a solve or a regression
included. `--kill-others` stops every GeometryDash process first (the Steam copy too), for a window
that has the screen to itself; use it only when nothing else is running.

## Sound

A solve makes no sound while it is solving: it runs hundreds of attempts a minute with the physics
tens of times faster than the song, so every note lands in the wrong place. The sound comes back
with the picture, for the showing of the solution and for any plain replay, and the song is then
kept with what is on screen — pausing the game pauses it, and the spectating notch retunes it up
to 4x (past that it is paused and re-seeked on the way back down).
