# Results

## v0.8.0: reference values

These are not the release's regression baseline. They are the latest runs in hand when this page was written: the
release candidate (v0.8.0-rc6), cold, the whole suite inside one game session, no plan and no solution file to start
from, on GD 2.2091 with Geode 6.0.0, measured on the lab machine — the machine the v0.7.0 numbers come from — so the
times compare with v0.7.0's. Every official and spin-off number below is a result of a package built on the lab machine and run in the game;
the package CI attaches to a release is a clang build, and was not what ran here.

### The 22 official levels

The whole suite twice: with every coin (`--cfg coinroute=1 coins=1`; every level ended with all three coins counted by
the game itself) and for the end of the level alone. A cell reads **with every coin**, then in brackets **the end of the
level alone**; repairs and times read v0.7.0 → v0.8.0 (what a repair is: see *What the numbers are* below).

| # | Level | Repairs, v0.7.0 → v0.8.0 | Time, v0.7.0 → v0.8.0 |
|--:|---|--:|--:|
| 1 | Stereo Madness | 0 → 0 (0 → 0) | 7 s → 9 s (5 s → 5 s) |
| 2 | Back On Track | 0 → 0 (0 → 0) | 4 s → 6 s (4 s → 4 s) |
| 3 | Polargeist | 0 → 0 (0 → 0) | 5 s → 6 s (5 s → 7 s) |
| 4 | Dry Out | 0 → 0 (0 → 0) | 5 s → 5 s (5 s → 4 s) |
| 5 | Base After Base | 0 → 0 (0 → 0) | 4 s → 5 s (4 s → 6 s) |
| 6 | Can't Let Go | 0 → 0 (0 → 0) | 5 s → 4 s (6 s → 6 s) |
| 7 | Jumper | 0 → 0 (0 → 0) | 5 s → 7 s (4 s → 5 s) |
| 8 | Time Machine | 0 → 0 (0 → 0) | 6 s → 5 s (5 s → 7 s) |
| 9 | Cycles | 0 → 0 (0 → 0) | 5 s → 5 s (5 s → 4 s) |
| 10 | xStep | 1 → 0 (0 → 0) | 10 s → 8 s (5 s → 6 s) |
| 11 | Clutterfunk | 0 → 0 (0 → 0) | 7 s → 10 s (36 s → 7 s) |
| 12 | Theory of Everything | 0 → 0 (0 → 0) | 5 s → 7 s (6 s → 5 s) |
| 13 | Electroman Adventures | 0 → 0 (0 → 0) | 5 s → 6 s (5 s → 6 s) |
| 14 | Clubstep | 0 → 0 (0 → 0) | 6 s → 8 s (6 s → 5 s) |
| 15 | Electrodynamix | 0 → 0 (0 → 0) | 6 s → 5 s (6 s → 6 s) |
| 16 | Hexagon Force | 1 → 1 (1 → 1) | 9 s → 15 s (8 s → 16 s) |
| 17 | Blast Processing | 1 → 1 (1 → 1) | 9 s → 11 s (9 s → 8 s) |
| 18 | Theory of Everything 2 | 1 → 1 (1 → 1) | 9 s → 15 s (9 s → 10 s) |
| 19 | Geometrical Dominator | 2 → 2 (1 → 2) | 24 s → 29 s (18 s → 40 s) |
| 20 | Deadlocked | 3 → 5 (2 → 2) | 32 s → 1 m 43 s (26 s → 24 s) |
| 21 | Fingerdash | 5 → 3 (1 → 1) | 1 m 0 s → 25 s (23 s → 19 s) |
| 22 | Dash | 13 → 10 (10 → 7) | 6 m 42 s → 5 m 22 s (4 m 37 s → 2 m 30 s) |

In total the 22 went from **27 to 23 repairs with coins and from 17 to 15 without**, and from 10 m 30 s to 10 m 16 s and from 7 m 57 s to 5 m 50 s.
With coins, three levels need fewer — xStep 1 → 0, Fingerdash 5 → 3, Dash 13 → 10 — and one more, Deadlocked, 3 → 5 (against v0.7.0; the trunk of 2026-11-24 needed 2, and the rise came with a later change to
how the repair ladder takes its first plan); for the end of the level alone, Dash 10 → 7 and Geometrical Dominator 1 → 2.

### The spin-off levels

The official levels of Meltdown, World and SubZero, and The Challenge, run the same way, one game session per set, for the end of the level alone (the six that have coins were
also run with every coin). These levels are not in the main game's level list; the suite reads them from level files (cfg `leveldir=<dir>`,
one `<id>.lvl` per level), which are not distributed here. A cell of the six that have coins reads **with every coin**, then in brackets **the end of the level alone**; the others have one number. Repairs and times read v0.7.0 → v0.8.0.

| Game | # | Level | Repairs, v0.7.0 → v0.8.0 | Time, v0.7.0 → v0.8.0 |
|---|--:|---|--:|--:|
| Meltdown | 1001 | The Seven Seas | 2 → 1 (3 → 1) | 14 s → 10 s (14 s → 10 s) |
| Meltdown | 1002 | Viking Arena | 1 → 1 (1 → 1) | 13 s → 10 s (13 s → 10 s) |
| Meltdown | 1003 | Airborne Robots | 2 → 1 (1 → 1) | 36 s → 33 s (12 s → 12 s) |
| World | 2001 | Payload | 1 → 0 | 6 s → 3 s |
| World | 2002 | Beast Mode | 0 → 0 | 5 s → 3 s |
| World | 2003 | Machina | 0 → 0 | 4 s → 2 s |
| World | 2004 | Years | 0 → 0 | 6 s → 5 s |
| World | 2005 | Frontlines | 0 → 0 | 7 s → 3 s |
| World | 2006 | Space Pirates | 0 → 0 | 6 s → 4 s |
| World | 2007 | Striker | 0 → 0 | 6 s → 4 s |
| World | 2008 | Embers | 3 → 3 | 5 s → 4 s |
| World | 2009 | Round 1 | 0 → 0 | 6 s → 3 s |
| World | 2010 | Monster Dance Off | 0 → 0 | 7 s → 4 s |
| — | 3001 | The Challenge | 1 → 0 | 7 s → 2 s |
| SubZero | 4001 | Press Start | 42 → 4 (15 → 1) | 8 m 5 s → 3 m 16 s (3 m 9 s → 25 s) |
| SubZero | 4002 | Nock Em | 7 → 7 (7 → 1) | 7 m 56 s → 7 m 22 s (1 m 41 s → 50 s) |
| SubZero | 4003 | Power Trip | 5 → 6 (5 → 5) | 1 m 10 s → 2 m 51 s (1 m 9 s → 1 m 30 s) |

The 17 went from **37 to 13 repairs** and from 7 m 43 s to 3 m 54 s; Press Start 15 → 1, Nock Em 7 → 1 and The Seven Seas 3 → 1 are most of it. The six that have coins, run with every coin (all three coins counted by the game in each), went from 59 to 20 repairs and from 18 m 14 s to 14 m 22 s. Each of these is one run; the spin-off levels have no baseline, so a level's count is not known to be reproducible.

### Custom levels

The custom levels were not measured again for this page; a reference count stands in. It is two measurements over the same lists of Daily, Weekly and Event levels (the 50 Daily levels as before, the current last 50 Weekly levels and all 29 Event levels, read on 2026-11-23: 129 levels) are put together here, and they are not one measurement. **(a) The re-measurement of 2026-11-23**: every level solved cold on the development trunk of that day, each given up to 1,200 s: 82 of the 129 cleared (Daily 43 of 50, Weekly 27 of 50, Event 12 of 29). **(b) Individual re-runs afterwards**: of the 12 levels that had cleared on an earlier build and did not in (a) (4 Daily, 4 Weekly, 4 Event), 8 were fixed and re-run on the v0.8.0-rc2 build, one game per level, each given up to 1,901 s (a longer limit than (a)'s 1,200 s, because these re-runs were measured on a slower machine than the re-measurement); all 8 cleared (4 Daily, 3 Weekly, 1 Event). **90 of 129** is (a) plus (b): no level was run again as a whole on one build with one limit, and the 4 not re-run in (b) are counted as not cleared. The levels are other people's work and appear only as a count, as in [CUSTOM_LEVELS.md](CUSTOM_LEVELS.md).

### What the numbers are

**Repairs** is how many times the loop had to go back: solve, replay, die,
re-anchor on the game's real state, solve the tail. `0` means the very first
plan the DP produced cleared the level. Each one is split by what the loop's
recorder found when it replayed the dead plan in the model against what the game
did:

* **divergence** — the model and the game had come apart before the death: the
  first difference the recorder found in position or velocity is over 0.01. The
  model is wrong there, and the repair is the game correcting it.
* **search** — the two agreed up to the death. The model, given what the game
  showed, dies there too; the plan was wrong, not the model — typically a route
  that only looked open because the moving geometry past the last replay was not
  yet recorded, or a plan that dies in the model's own walk of it.

On the official levels of v0.7.0's run, all 17 repairs without coins and 26 of the 27 with
them were search. For a given build the count is normally reproducible: `py/cold_regress.py` prints
each level's count against `data/cold_baseline.json`
(`data/cold_baseline_coins.json` for a coin run), and a change is reported, not
failed. The `[fp]` line each round prints is compared with the baseline run's
separately when a release is accepted.

It is not a fidelity score. It counts what the loop had to do, and that depends
on which corridor the search happens to walk as much as on where the model is
wrong — the search is deepest-first, so touching one rule reorders the frontier
and the run takes a different route. Making the model *more* correct can raise
it: closing a route the model only believed in sends the search off to find the
real one.

**Time** counts everything from the level being built to the solution being written (the game's own start-up is
not in it). Read the clock as a guide and not as a contract: it is not what the regression compares, and a
level's time is read from outside the game about once a second.
