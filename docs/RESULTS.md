# Results

The tables are cold regression runs of the v0.7.0 build (2026-10-24) on GD 2.2091,
the executable Steam has had since 2026-10-17 —
`python py/cold_regress.py --one-session`, the whole suite inside a single game
session, no plan and no solution file to start from — one with coins
(`--cfg coinroute=1 coins=1`) and one for the end of the level alone. The coin
runs ended every level with every coin, as the game itself counted them. The
repairs and times are those of the runs the regression baseline was taken from.

Every cell reads **with every coin**, then in brackets **the end of the level
alone**. A repair cell splits into **divergence + search** (see below).

| # | Level | Repairs | Time | | # | Level | Repairs | Time |
|--:|---|--:|--:|---|--:|---|--:|--:|
| 1 | Stereo Madness | 0 (0) | 7 s (5 s) | | 12 | Theory of Everything | 0 (0) | 5 s (6 s) |
| 2 | Back On Track | 0 (0) | 4 s (4 s) | | 13 | Electroman Adventures | 0 (0) | 5 s (5 s) |
| 3 | Polargeist | 0 (0) | 5 s (5 s) | | 14 | Clubstep | 0 (0) | 6 s (6 s) |
| 4 | Dry Out | 0 (0) | 5 s (5 s) | | 15 | Electrodynamix | 0 (0) | 6 s (6 s) |
| 5 | Base After Base | 0 (0) | 4 s (4 s) | | 16 | Hexagon Force | 0+1 (0+1) | 9 s (8 s) |
| 6 | Can't Let Go | 0 (0) | 5 s (6 s) | | 17 | Blast Processing | 0+1 (0+1) | 9 s (9 s) |
| 7 | Jumper | 0 (0) | 5 s (4 s) | | 18 | Theory of Everything 2 | 0+1 (0+1) | 9 s (9 s) |
| 8 | Time Machine | 0 (0) | 6 s (5 s) | | 19 | Geometrical Dominator | 0+2 (0+1) | 24 s (18 s) |
| 9 | Cycles | 0 (0) | 5 s (5 s) | | 20 | Deadlocked | 0+3 (0+2) | 32 s (26 s) |
| 10 | xStep | 0+1 (0) | 10 s (5 s) | | 21 | Fingerdash | 1+4 (0+1) | 1 m 0 s (23 s) |
| 11 | Clutterfunk | 0 (0) | 7 s (36 s) | | 22 | Dash | 0+13 (0+10) | 6 m 42 s (4 m 37 s) |

The two numbers in a cell are two separate searches, not a total and a part:
with coins the goal is a different one, so the search takes a different route
from the first round on, and it is not always the longer one.

Against v0.6.0's one-session runs, the 22 levels went from 27 to 17 repairs and
519 to 477 s without coins, and from 33 to 27 repairs and 611 to 630 s with them.
The large moves: Dash 16 → 10 without coins and 17 → 13 with them, Fingerdash
7 → 5 with coins and 2 → 1 without. Against the run of the trend, Deadlocked went
2 → 3 with coins. The [changelog](../changelog.md) has what changed.

### The spin-off levels

The official levels of Meltdown, World and SubZero, and The Challenge, run the
same way, one game session per set. World's levels and The Challenge have no
coins, so they have one number. These levels are not in the main game's level
list; the suite reads them from level files (cfg `leveldir=<dir>`, one
`<id>.lvl` per level), which are not distributed here.

| Game | # | Level | Repairs | Time |
|---|--:|---|--:|--:|
| Meltdown | 1001 | The Seven Seas | 0+2 (0+3) | 14 s (14 s) |
| Meltdown | 1002 | Viking Arena | 0+1 (0+1) | 13 s (13 s) |
| Meltdown | 1003 | Airborne Robots | 0+2 (0+1) | 36 s (12 s) |
| World | 2001 | Payload | 1+0 | 6 s |
| World | 2002 | Beast Mode | 0 | 5 s |
| World | 2003 | Machina | 0 | 4 s |
| World | 2004 | Years | 0 | 6 s |
| World | 2005 | Frontlines | 0 | 7 s |
| World | 2006 | Space Pirates | 0 | 6 s |
| World | 2007 | Striker | 0 | 6 s |
| World | 2008 | Embers | 0+3 | 5 s |
| World | 2009 | Round 1 | 0 | 6 s |
| World | 2010 | Monster Dance Off | 0 | 7 s |
| — | 3001 | The Challenge | 0+1 | 7 s |
| SubZero | 4001 | Press Start | 7+35 (0+15) | 8 m 5 s (3 m 9 s) |
| SubZero | 4002 | Nock Em | 3+4 (0+7) | 7 m 56 s (1 m 41 s) |
| SubZero | 4003 | Power Trip | 0+5 (0+5) | 1 m 10 s (1 m 9 s) |

Against v0.6.0, the 17 levels without coins went from 21 to 37 repairs and 259 to
463 s, and the six with coins from 18 to 59 repairs and 629 to 1,094 s. Nearly all
of it is SubZero: Press Start 2 → 42 with coins and 2 → 15 without, Nock Em 5 → 7
with coins and 3 → 7 without. Press Start had stopped getting its first coin at
all before this release's reverse-sync fix; with the fix it takes every coin, and
the repairs are the search walking into deaths the model agrees with, one per
round. The rest of the spin-off levels are within a repair of v0.6.0. Each of
these is one run; the spin-off levels have no baseline, so a level's count is not
known to be reproducible.

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

On the official levels all 17 repairs without coins and 26 of the 27 with them
are search; on the spin-off levels 36 of 37, and 49 of 59 with coins. For a
given build the count is normally reproducible: `py/cold_regress.py` prints
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

**Time** counts everything from the level being built to the solution being
written (the game's own start-up is not in it), on a 16-core desktop; the 22 add
up to 10.5 minutes with coins and 8 without. No other game ran beside them. Read the
clock as a guide and not as a contract: it is not what the regression compares,
and a level's time is read from outside the game about once a second.

Almost all of it is the search. Broken down on a run before the section solver
joined the loop, the DP calls were 86 % of the total and 88–93 % on the four
expensive levels; on the ones that finish inside half a minute, most of what is
left is starting the game and loading the level. On a level where section solves
run (Dash with coins, SubZero) a good part of the time is spent searching inside
the game instead.
