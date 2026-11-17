# Custom levels

Nothing in the loop is specific to the official levels — level selection is the
game's own, and the level model is built out of whatever `PlayLayer` loaded — so
any level can be picked, and most custom levels built from parts that existed
before 2.0 solve as they are. What custom levels do not have is a guarantee: the
regression covers the official levels only, and the physics is measured against
what those levels, and the calibration rigs in `data/rigs`, use. A custom level's
result is whatever the game lets the loop do on it.

A level that leans on a mechanic the model has not been measured against is a
level the model is wrong about. Being wrong does not produce a wrong answer —
every plan is flown by the game, and only a plan the game clears is saved — but
it is expensive: the loop finds each disagreement by dying in a replay and
repairs one at a time, so an unmodelled level does not fail, it crawls.

The rule is to clear what the game lets the loop clear. Where the model has no
answer — a mechanic it lacks, a stretch it cannot represent — the section solver
drops it and searches the game itself ([SECTION_SOLVE.md](SECTION_SOLVE.md)), and a
plan the game clears is a solution however it was found. The difference is in the
cost: a level the model covers is fast and predictable; one that clears only with
the section solver's help is slower, and may take a different route each time.

This page says what that looks like in practice: which levels usually solve,
which gimmicks the model does not cover, and what a failure looks like. No level
of the survey is named; the levels measured are other people's work.

## Trying one

Open the level's own page in the game, press its play button and pick **Solve**,
as for an official level. Platformer levels get no menu (below). The safety rules
are the same as everywhere: while the mod drives, nothing is recorded.

## What the measurements say

100 rated levels drawn at random from the level server, from every game version,
are solved cold with each release build — the same loop as the official ones,
each level given up to 20 minutes. On the v0.5.0 build **93 of the 100 cleared**
(v0.4.0: 67). Grouped by the game version the level was last saved in:

| saved in | parts it can contain | levels | cleared (v0.4.0) | the rest | rounds (median, cleared) |
|---|---|--:|--:|---|--:|
| before 1.9 | cube, ship, ball, UFO, slopes and dual; the early portals, pads and rings | 3 | 3 (3) | — | 0 |
| 1.9 | wave added | 10 | 10 (10) | — | 2.5 |
| 2.0 | robot, teleports, move and toggle triggers | 9 | 8 (8) | 1 ran out of time | 3.5 |
| 2.1 | spider, dash rings, many triggers | 49 | 46 (35) | 3 ran out of time | 6.5 |
| 2.2 | swing, area effects, many more triggers | 29 | 26 (11) | 2 ran out of time, 1 stopped at once (likely a game bug) | 10.5 |
| all | | 100 | 93 (67) | | |

The level that stopped at once did so in a way that points to a bug on the game's
side rather than to the solver; it stays in the hundred as a level that did not
clear. The 20 minutes are the measurement's time box, not a verdict: the
levels that ran out of time were still making progress, and the runs share one
machine, several levels at a time, so a level close to the limit can land on
either side of it. The hundred was not measured again for v0.6.0 or v0.7.0.

### The Daily, Weekly and Event levels

For v0.6.0 a second survey took the levels the game itself features: 50 drawn at
random from the last 365 Daily levels, the last 50 Weekly levels (about a year of
them; Weekly levels are demons), and every Event level in the game's Safe list. Each was
solved cold at the release defaults and given up to 20 minutes, on GD 2.2081
with a build from just before the release (before the rules turned on by default
and the switches removed in v0.6.0). 74 of the 126 cleared.

The same 126 were solved again on the v0.7.0 build, on GD 2.2091, with the same
20 minutes each and four levels at a time: **77 of the 126 cleared.** Twenty-two
of them are also the custom levels the regression runs before every release; those
were not run again, and their result is that of the regression run of this
release's model fixes, which gave each level 25 minutes.

| list | levels | cleared | ran out of time | stopped | median time to clear | cleared on v0.6.0 |
|---|--:|--:|--:|--:|--:|--:|
| Daily | 50 | 42 | 8 | 0 | 133 s | 41 |
| Weekly | 50 | 24 | 26 | 0 | 444 s | 23 |
| Event | 26 | 11 | 14 | 1 | 245 s | 10 |
| all | 126 | 77 | 48 | 1 | | 74 |

The median is over the levels this survey ran itself.

These lists are chosen by the game's developer, and the Weekly ones are demons,
so the two surveys draw from different pools and their rates are not comparable.

The version a level was saved in is only a guess at what it uses: a level saved in
2.1 may hold nothing newer than 1.8. On the v0.4.0 build the survey also took 200
levels saved in 1.8 or earlier (the second hundred only 1.8 levels with slopes and
no dual), which cleared 95 and 96. Over all 300, grouped by the newest object a
level actually holds (each object dated by the oldest game version of a server
level that contains it), the 215 levels with nothing from 2.0 on cleared 206 times;
the 85 with an object from 2.0 on, 52 times. That is the line the play menu's
yellow note draws.

On the v0.4.0 build, where the model and the game came apart, per 10,000 ticks the game flew (each
level's deepest attempt; a place counts once however many rounds died there, and a
death where only the kill differed counts once the model, replayed on its own,
has confirmed it — the few that could not be checked are left out):

| the level holds | all | cube | ship | ball | UFO | wave | robot | spider | swing |
|---|--:|--:|--:|--:|--:|--:|--:|--:|--:|
| nothing newer than 1.8 | 0.45 | 0.28 | 0.29 | 1.00 | 1.01 | — | — | — | — |
| 1.9's wave, nothing from 2.0 on | 1.17 | 0.77 | 0.75 | 2.39 | 2.00 | 0.99 | — | — | — |
| an object from 2.0 on | 2.57 | 1.23 | 3.46 | 4.52 | 2.72 | 11.91 | 2.32 | 1.99 | 34.1 |

The ten wave levels are a small sample; their rates rest on 18 places. On the
levels with newer objects the wave is the outlier, and the swing has barely been
measured at all.

*When and with what:* the hundred, 2026-10-11 on the v0.5.0 build and 2026-10-02
on v0.4.0; the 200 older levels and the rates per mode, 2026-10-02 on v0.4.0; the
Daily, Weekly and Event levels, 2026-10-16 and 10-17 on GD 2.2081, and again
2026-10-25 on GD 2.2091 with the v0.7.0 build. Four of them
were run again on 10-17, on builds with the same defaults, after their first run was
cut short (one never started, one was left by hand, one was stopped for memory, one
was stopped early by the test harness).

## Levels that usually solve

* Built from **parts that existed before 2.0**: cube, ship, ball, UFO and wave,
  slopes and dual, the early portals (gravity, size, speed, mirror, mode),
  yellow/pink/blue pads and rings, blocks, spikes and saws.
* **Not too long and not too heavy.** Every round replays the level from the
  start, and the model is built from every object; a few thousand objects is
  typical of the levels above.
* Where the model has been measured most: on those levels the **cube** and the
  **ship** disagreed with the game least often, the ball and the UFO three to four
  times as often, and most disagreements were in mid-air or next to a ring.

## Gimmicks the model does not cover

### Refused

* **Platformer mode.** Both searches — the model's and the section solver's — rest
  on there being no steering input, one bit per tick, so a platformer level is
  outside the formulation. The mod refuses one (see the README's *What's next*).
* **More than 128 gravity portals**, in a level that reverses the player or puts
  them close together. Past 128 the portals share the model's slots, which works
  only when each is behind the player for good before the next one needs it; a
  level where that does not hold is refused, and the log says why
  (`unsupported: gravity portals ...`).

### Not in the model: left to the section solver

The model cannot plan through these, so the loop can get past them only where the
section solver finds a way through the game itself — slowly, and not always.

* **Some teleports.** The teleport portal, the teleport trigger and the teleport
  orb are modelled, as are a portal that pushes the player or keeps their height,
  an exit anywhere in x — ahead, above or below, or behind, where the triggers
  already passed do not fire again, as in the game — and a teleport trigger set
  off by touch, by a spawn the player's crossing or touch starts, or by an Event
  trigger listening for the player's pads and orbs. Not yet: an orb that pushes
  the player, an exit the game draws from several objects, a teleport trigger that
  keeps the player's offset, and one set off by an item, a delayed spawn or any
  other event.
* **Some gravity triggers.** The gravity trigger (2.2) is modelled when the player
  sets it off by crossing or touching it, by starting the spawn that fires it, or
  through an Event trigger listening for the player's pads and orbs; one for
  player 2 in a level with a second player, or one set off by an item, a delayed
  spawn or any other event, is flown by the game with the new gravity and planned
  by the model with the old.
* **Some Follow Player Y triggers**: one with a delay, one set off by touch or a
  spawn, or more of them running at once than the model's state holds -- named
  only when the group they move holds something with a body (a solid, a hazard, a
  portal, a pad, an orb); a group of decoration alone changes nothing the player
  meets.
* **Two-player levels where one player's press moves the other's world.** The
  solver plans both players' buttons, each body against the other's plan, but a
  level where a press of one is a switch the other's way depends on (walls or doors
  flipped at any moment) is not solved yet, and the section solver is never used
  on a two-player level ([TWO_PLAYER.md](TWO_PLAYER.md)). The play menu names a
  two-player level in yellow.
* **Levels that change from one attempt to the next.** An Item Compare or Item
  Edit can read the attempt count, and Item Persistence keeps items across
  attempts, so a level can, say, skip its intro from the second attempt on. The
  loop records the moving geometry on one attempt and flies its plans on later
  ones, and a solution replayed as a first attempt meets a different level. Three
  settings, off by default, pin what such a level reads, so that every attempt
  meets the same level: `attemptone` (every bot attempt is the level's first),
  `attemptcount=k`, and `persistworld=k` / `persistitems` (every reset starts from
  one fixed table of kept items). Many levels count attempts only to show
  something — the attempt number on a label, a message on the first try — and
  the play menu warns about a level only when what changes reaches something
  the player touches or the player itself.
* **Randomness.** While the bot drives, the game's random seeds are set to the same
  values before every attempt, so each attempt meets the same draw — an Area Move's
  distance, a Random trigger's choice — and the model plans against the positions
  the game's own recording shows. A solution is one for that draw: played without
  the mod, or by another tool, it can meet other positions. The play menu says
  when a level has a Random or Advanced Random trigger, or an area or enter effect
  with a random variance (a spawn delay's or an Advanced Follow's variance is not
  detected yet). The section solver's checkpoint restores start it from the
  attempt's first values too, so a stretch it solves after a Random trigger can
  still fail on the replay from the start, which then refuses it.

### In the model, but measured less

* **Slopes**, above all where they meet other slopes, blocks or the ceiling.
* **The wave** against plain floors, ceilings and slides, and on the levels with
  newer objects generally — its rate in the table above is the highest of the
  modes that have been measured much.
* **Mechanics no official level uses.** The model is measured one mechanic at a
  time against the official levels and the rigs; where a mechanic-and-mode
  combination has never been measured, the code carries a fallback that says so
  in a comment. The 2.x triggers are where most of these live.
* **Dual sections** beyond the few the official levels use: a dual ball in one
  custom level is known to disagree with the game.

### Too much for the model's memory

The model's state is small on purpose. A rule that needs one bit per object, for a
kind of object a level places by the hundred — a pad the game fires only once per
attempt, repeated hundreds of times; blocks that break and must stay broken — cannot
be carried. Such a stretch is left to the section solver, which uses the game's own
physics and gets through where the model cannot follow.

### Very heavy levels

Levels with tens of thousands of objects (in an earlier sample of 2.x levels the
median was about 26,000, the largest over 150,000) are slow: the game's own update, the recording of the
moving geometry and the model all grow with the object count. A level with at least
10,000 objects its run cannot depend on is solved on a copy without them and
verified on the level itself ([LEVEL_SLICE.md](LEVEL_SLICE.md)), which helps; a
level whose gameplay objects are themselves that many is still slow.

## What a clear and a failure look like

A clear is a plan the game flew from start to finish, whether the model planned
every stretch of it or the section solver searched some. The log says which: each
stretch the section solver found is a `secrung: spliced ...` line.

* **Refused** — the session ends at once and says what it cannot handle.
* **No first plan** (`dpsolve_stuck` at the start) — the model finds no way through
  from the very beginning, often because of a mechanic it does not have.
* **Gives up at a wall** — the loop keeps dying at one place and runs out of things
  to try there. The iteration map (`F10`, [ITERMAP.md](ITERMAP.md)) shows where the
  rounds went, and whether the model or the plan was wrong there.
* **Runs out of time** — in the surveys, still making progress when the level's
  clock ran out; a crawl.

In every case nothing wrong is saved: a solution is only ever a plan the game
itself cleared.

## Coins

The coin routing is measured on the official levels: the pickup box, and the
counters, keys and gates their coins wait for. A custom level's coins can hang on
triggers the official ones never use, and the search holds at most eight coins — a
level with more turns the routing off and says so. See [COINS.md](COINS.md).

## What is being worked on

* **Model divergences** — each rule that disagrees with what the game measures is
  fixed, one at a time; this never stops.
* **The search's logic** — plans that die where the model is right. On the
  official levels most repairs are of this kind (see [RESULTS.md](RESULTS.md)).
* **Custom levels in the regression**, so that a change is measured on them and
  not only on the official levels.
* **Heavy levels** — the speed of levels with very many objects.

Whatever a level uses, the loop tries, and a level clears if the game lets it.
The play menu says up front where that is less likely
([RUNNING.md](RUNNING.md#the-play-menu)): a red
line when a level holds something the model cannot express (the teleports,
gravity and follow triggers above, or a level that changes from one attempt to
the next), and a yellow one on a two-player level and on a custom level with objects
from 2.0 on. What is
still planned is saying it afterwards too: a clear reported as either the
model's alone or one with stretches the section solver found.
