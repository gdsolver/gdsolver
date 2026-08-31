# gdsolver

**An automatic TAS solver for Geometry Dash** (2.2081, Windows / Steam). It
solves a level by playing it — inside the game, from cold.

<p align="center">
  <img src="docs/media/solve-to-replay.webp" width="720"
       alt="gdsolver solving Dash: the dark fast loop with the overlay counters climbing, then the screen coming back for the solution replayed at 1x">
</p>

<p align="center"><sub>
  The last five seconds of solving <b>Dash</b>, then the first seven of the
  replay. The overlay is the loop's own: which round it is on, how far the best
  plan has reached, how far the search has got. The screen is dark because the
  search is running — the frames go to the fast loop instead — and it comes back
  when a candidate has actually cleared.
</sub></p>

Give it a level and it finds an input sequence that clears it. It does not
re-implement the game's physics: an approximate, measured-where-it-matters model
drives a **reachability DP** (which `(y, velocity)` states are reachable at every
physics tick), and the game itself — running inside a **Geode mod** — is the
verifier that decides whether a candidate plan really clears. Where the replay
diverges from the model, the solver re-anchors on the game's own state and
continues from there.

**All 22 official levels are solved cold** — no seeds, no previous solutions, no
external ground truth — by the loop inside the game, with nothing installed but
Geometry Dash, Geode and this mod. Pick **Solve** on the panel and it builds the
level, searches, replays the candidate, and repairs the plan wherever the game
disagrees with it; no external process is involved. Most levels take under a
minute. The solutions are tracked in [`data/`](data/) as
`solution_lv<N>_dp.txt`, one `input=<tick>,<0|1>` line per press or release.

Those 22 are also the supported set — see [What's next](#whats-next) for where
custom levels, platformer mode and coins stand.

## Results

The whole table is one cold regression run — `python py/cold_regress.py`, one
game session per level, no plan and no solution file to start from — and it is
run again on every change that reaches the loop.

| # | Level | Repairs | Time | | # | Level | Repairs | Time |
|--:|---|--:|--:|---|--:|---|--:|--:|
| 1 | Stereo Madness | 0 | 17 s | | 12 | Theory of Everything | 3 | 13 s |
| 2 | Back On Track | 1 | 13 s | | 13 | Electroman Adventures | 0 | 15 s |
| 3 | Polargeist | 0 | 14 s | | 14 | Clubstep | 7 | 30 s |
| 4 | Dry Out | 0 | 12 s | | 15 | Electrodynamix | 2 | 19 s |
| 5 | Base After Base | 3 | 16 s | | 16 | Hexagon Force | 55 | 4 m 13 s |
| 6 | Can't Let Go | 0 | 12 s | | 17 | Blast Processing | 1 | 25 s |
| 7 | Jumper | 2 | 23 s | | 18 | Theory of Everything 2 | 8 | 40 s |
| 8 | Time Machine | 1 | 19 s | | 19 | Geometrical Dominator | 18 | 1 m 34 s |
| 9 | Cycles | 3 | 16 s | | 20 | Deadlocked | 37 | 6 m 55 s |
| 10 | xStep | 6 | 27 s | | 21 | Fingerdash | 30 | 3 m 47 s |
| 11 | Clutterfunk | 1 | 18 s | | 22 | Dash | 57 | 7 m 48 s |

**Repairs** is how many times the loop had to go back: solve, replay, die,
re-anchor on the game's real state, solve the tail. `0` means the very first
plan the DP produced cleared the level. The count is deterministic for a given
build, which is why it is what `py/cold_regress.py` compares against
`data/cold_baseline.json`.

It is not a fidelity score. It counts what the loop had to do, and that depends
on which corridor the search happens to walk as much as on where the model is
wrong — the search is deepest-first, so touching one rule reorders the frontier
and the run takes a different route. Making the model *more* correct can raise
it: closing a route the model only believed in sends the search off to find the
real one.

**Time** is one run (2026-08-29), each level solved on its own — one game session
at a time, 8 solver threads, 16-core desktop — and it counts everything from
launching the game to the solution being written. The whole suite takes 30
minutes that way. Read it as a guide and not as a contract: the clock is not what
the regression compares, the repair count is, and only that one is deterministic.
Solving four levels at once — which is how the suite is normally run — costs each
expensive level 34–49 % more search time and moves none of the counts.

Almost all of it is the search. Broken down on the 2026-08-27 run, the DP calls
were 86 % of the total and 88–93 % on the four expensive levels; on the ones that
finish inside half a minute, most of what is left is starting the game and
loading the level. That split has not been re-measured since — the suite wipes
each session's log before the next level, so it needs its own run — but nothing
about where the time goes has changed.

## How it works

```mermaid
flowchart LR
    LV["a level<br/>objects, triggers, moving geometry"]
    subgraph GD["Geometry Dash + Geode"]
        direction TB
        MOD["src/ — the mod<br/>exports the level, replays a plan in a fast<br/>headless loop, dumps the state every tick"]
        DP["dp/ — reachability DP<br/>layered search over 240 Hz ticks<br/>exact states, dedupe on a quantised key"]
        MOD -- "level model" --> DP
        DP -- "candidate plan" --> MOD
    end
    PY["py/ — starts the game and measures it<br/>cold regression, replay verification,<br/>fidelity diagnostics, calibration rigs"]
    LV --> GD
    GD --> OUT["solution<br/>one line per press and release"]
    PY -.-> GD
```

Geometry Dash gives the player no steering input: the level sets the forward speed,
so the only decision per physics tick is *pressed or not*. Clearing a level is
then a reachability question over `(tick, y, vy, mode, gravity, size, ...)`, and
two properties make the game usable as the oracle for it: the physics are
deterministic under *click on steps*, and x is essentially a clock, so re-anchoring
the search on a later tick costs nothing. (*Essentially*, because stair snapping
leaves a sub-pixel phase and a rotation section turns the frame — see
[docs/ARCHITECTURE.md](docs/ARCHITECTURE.md#1-the-problem).) That is the whole loop:

```mermaid
flowchart TD
    S["build the level model from the loaded level"]
    S --> D["solve from the current anchor"]
    D --> R{"replay the plan in the game"}
    R -- clears --> W["solution saved"]
    R -- dies --> F["find the first tick where the game and the model<br/>disagree, and record it as a fixup"]
    F --> A["re-anchor: take the player state the game really had<br/>a few ticks before the death"]
    A --> D
```

A *fixup* is the model agreeing to be wrong: from then on, whenever it meets
that same state again it applies the game's result instead of its own. Dead
bands, touch triggers that must be entered, ladders of anchor depths and
phantom-death detection exist for the other cases where the model is blind.
Every run is cold, and a run's fixups live only in that run.

There is also a **section solver**, which drops the model for one stuck section
and lets the game itself be the transition function, searching forward from a
practice-mode checkpoint. It is deliberately *not* part of the loop above:
nothing starts it on its own, it does not splice its answer back into the plan,
and it ends the run when it finishes. It is a tool for interrogating one wall by
hand, and no official level has needed it —
[`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md#31-the-section-solver) has the
mechanism, its costs, and the kind of death it cannot see.

* **`dp/`** — the solver core (`leveldp`, also linked into the mod). States are
  *exact*; the per-layer hash only deduplicates and never snaps a state, so every
  surviving path is a replayable plan. The DP is a candidate generator — the
  proof is always a plain replay in the game. Its physics rules are measured
  against the game (calibration levels, traces, disassembly), not fitted.
* **`src/`** — the Geode mod: level export, input injection, the per-tick state
  dump, moving-geometry recording, the fast headless loop, and the repair loop
  itself (`src/mod/repair.hpp`). It also blocks achievements and statistics
  while it is driving.
* **`py/`** — development tools; nothing here solves. The cold regression over
  all levels (`cold_regress.py`), replay verification, fidelity diagnostics,
  calibration-level generation, and an MCP server for probing the running game.

[`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md) has the long version.

## What's next

**Custom levels — unsupported today, partial support planned.** Nothing in the
loop is specific to the official levels: level selection is the game's own, the
level model is built out of whatever `PlayLayer` loaded, and plenty of custom
levels already solve as they are. They are nonetheless **not supported**: the
difference between "it worked" and "it is expected to work" is the whole gap. The
physics is measured on the calibration rigs in `data/rigs`, one mechanic at a time
across the game modes, and wherever a mechanic-and-mode combination has never been
swept the model carries a fallback that says so in a comment. A level that leans on
one of those is a level the model is wrong about — and being wrong is expensive
here: the loop finds out only by dying in the replay, and repairs one disagreement
at a time. The regression covers the official levels only, so nothing else is being
kept honest. The work is to pick a subset that is genuinely supported, say
plainly which objects are in it, and make a level outside it fail loudly instead
of slowly.

**Platformer levels are outside the formulation, not merely unmeasured.** The
search rests on there being no steering input; a platformer level hands the player
one, so forward position stops being driven by the level and the decision each
tick stops being one bit. No measurement closes that — it is a different search
problem. So the mod refuses one instead of answering: it reads both the layer's
flag and the level's own, and takes the refusal if the two ever disagree. Being
wrong slowly is the thing this project is built to avoid, and an answer nobody can
check is worse than a sentence saying there is no answer. Safety is unaffected
either way — the record gate asks whether an automated session is open, never which
mode is running.

*What to expect, roughly.* The official suite is 1.x apart from two levels —
Fingerdash is 2.0 and Dash is 2.2 — so that is the era the model has been
measured against, and the repair counts in the table above are the visible half
of it: the earliest levels need 0 to 3 rounds, and the four most expensive are
all late ones. The expectation for custom levels follows: one built out of
early-era parts stands a good chance, a recent one much less.

Read that as a claim about *parts*, not about dates. What costs rounds is a
mechanic the model has not been measured against, and mechanics arrived with
versions, which is the only reason the era works as a proxy at all — and the
order does not actually follow the versions. Dash, which is 2.2, is the most
expensive at 57 rounds; Hexagon Force, which is 1.6, is right behind it at 55,
and it costs that because of its dual section; Fingerdash, in between at 2.0,
needs 30, and Blast Processing, two versions after Hexagon Force, needs 1. A 2.2
level that happens to be a plain cube level may well solve on the first plan,
and a 1.6 level with a dual may not.

**Coins.** The objective today is "reach the end alive", so a solution picks up a
coin only by accident. The level export always records where the coins are, and
there is a pickup test that does not consult GD's own coin state, so it still
works while every award is blocked — though that test is off unless asked for
(`coinMode`), because nothing in the solve needs it yet. What is missing is the
search: it does not route through them. Making coins an objective means carrying
"which coins are taken" in the
search state, and accepting that the cheapest route to the end is usually not a
route that passes every coin. Coins collected while the mod drives are never
awarded (see [Safety](#safety--community-notes)) — a coin route is a TAS result,
not progress on a save file.

## Installing

Grab **[`gdsolver.solver.geode` from the latest release](https://github.com/gdsolver/gdsolver/releases/latest)**
and drop it into Geometry Dash's `geode/mods/` folder, then start the game — the
usual place is

```
C:\Program Files (x86)\Steam\steamapps\common\Geometry Dash\geode\mods\
```

Geode itself has to be installed first ([geode-sdk.org](https://geode-sdk.org)).
The build is for **Windows and GD 2.2081**; Geode refuses to load it on anything
else rather than misbehaving. Nothing else is needed — no Python, no external
process, no second copy of the game.

The mod announces itself in the log the moment it loads, and says what it
suppresses:

```
gdsolver: loaded. While the bot drives -- solving or replaying -- achievements,
statistics, coins and the level's own record are all blocked. Your own attempts,
with the bot idle, record as usual.
```

Installing it does **not** cost you your progress: the block is tied to an
automated session being open, not to the mod being present. See
[Safety](#safety--community-notes).

## Building

Requirements: Visual Studio 2022 (C++ desktop workload), CMake ≥ 3.21, the
[Geode SDK](https://geode-sdk.org) (`GEODE_SDK` environment variable) and
Geode CLI for the mod.

```powershell
# solver CLI only (no Geode needed)
cmake -S . -B build-dp -DGDSOLVER_BUILD_MOD=OFF
cmake --build build-dp --config Release        # -> build-dp/dp/Release/leveldp.exe

# mod + CLI
geode build                                    # -> build/gdsolver.solver.geode
```

## Watching a run

A panel appears on the screens a level is started from, offering three modes: **Normal**,
**Replay** (play a stored solution) and **Solve** (solve the level in-process). A solve runs
fast, dark and silent until a candidate clears, and then plays that one through at 1x with the
artwork and the music. While a level is on screen an overlay reports what the loop is doing, and
F10 draws the iteration map: where the repair rounds went, and where the model — rather than
the plan — was wrong.

The overlay has no key that lets a player do what the game does not allow: no warp, no noclip,
no forced mode, no infinite jump. It is for watching a run, not for changing one.

[docs/RUNNING.md](docs/RUNNING.md) has the panel, the overlay, the full key table and the seek
bar; [docs/ITERMAP.md](docs/ITERMAP.md) has what the map's marks mean.

## Safety / community notes

This is a TAS and research tool, not a way to fake records.

* **While the mod is driving — solving or replaying — nothing is recorded**:
  achievements, statistics and coins are blocked, and so is the level's own
  record (completion percentage, normal/practice best, attempts, jumps, orbs,
  diamonds, the secret key, coin achievements). GD's own "do not record" path
  is used for the block — the flag it sets when a level is played from a start
  position — and the counters GD advances outside that path (the level's
  attempt and jump counts) are put back afterwards.
* **Playing it yourself records normally.** The suppression is tied to an
  automated session being open, not to the mod being installed, so with the mod
  loaded but idle your own attempts, progress and achievements count as usual.
* Every session ends with the evidence in its log: how many recordings were
  blocked, and a `level record changed:` line that names any field that moved.
  It should read `none`.
* Replays driven by the mod are marked as bot input.
* The level-data dumps of the official levels that the development workflow
  uses are not part of this repository; the mod regenerates them from your own
  copy of the game.

## License

MIT — see [LICENSE](LICENSE).
