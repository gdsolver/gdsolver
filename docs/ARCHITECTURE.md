# Architecture

This document describes how gdsolver finds an input plan for a level and which
component owns which part of the job. The loop runs in-process — Geometry Dash,
Geode and the mod, nothing else — which is what §4 is about.

## 1. The problem

Geometry Dash gives the player no steering input: the level sets the forward
speed (speed portals included), so the only decision per physics tick
(240 Hz) is *pressed or not*. The state that matters is
`(tick, y, y-velocity, mode, gravity, size, ...)`, and clearing a level is a
reachability question: is there a sequence of presses under which the player
is alive at the end?

Forward position is *nearly* the clock rather than exactly it, and both exceptions
are visible in the search key. GD's stair snapping leaves a sub-pixel phase —
landings differ by 0.3 to 1.0 px, and one of those decides whether the next landing
falls on tick n or n+1 — so `xAbs` is keyed at a quarter pixel (`search_key.hpp`,
measured on lv11 x=24,863, where the only route lands at 24863.4 and jumps on the
very next tick). And a 2.2 rotation section turns the whole gameplay frame, so
world x can freeze for a stretch while the frame's own forward axis keeps advancing
— lv22's reference trace at t=16,995..17,005 holds x at 20085.0996 while y climbs
1.95 a tick under `gframe=3` (`frames.hpp`); the model holds its state in the
current frame's coordinates so that every rule stays written as "x is the clock,
y is height". Neither exception
hands the player an axis to steer with, and that — not "x is the tick" — is the
property the formulation rests on. A platformer level does hand one over, which is
why those are out of scope rather than merely unmeasured.

Two facts make the game a usable oracle for that question:

* **Determinism** — with input precision set to *click on steps*, the same input
  sequence yields the same trace, independent of frame rate. This is measured, not
  assumed; everything below rests on it. It holds for fixed settings, and one
  graphics setting is not neutral: the resolution option changes the hit radius
  the game gives saws (lv20's id 187 is 21.87 at resolution index 25 and 21.96 at
  index 8), which is enough to change a cold run's first plan. So traces, plans and
  baselines compare only at one resolution: the results here are measured at
  index 25, and the cold regression records the index and refuses to compare
  across it.
* **x is essentially a clock** — re-anchoring the search on a later tick costs nothing in
  terms of path choice, so a plan can be verified in the game and repaired
  from the first point of disagreement.

## 2. Components

```mermaid
flowchart TB
    subgraph loop["in the game — the loop is src/mod/repair.hpp"]
        direction TB
        MOD["src/ — Geode mod (gdsolver)<br/>level-data export · headless fast loop · input injection<br/>per-tick state dump · moving-geometry recording<br/>serve mode (file protocol) · section solver<br/>safety (stats blocked)"]
        DP["dp/ — reachability DP (leveldp)<br/>layered search over ticks · exact states<br/>dedupe on a quantised (y, vy, x-phase, mode, ...) key<br/>per-mode physics rules measured against the game"]
        MOD -- "level model, re-anchor state, fixups" --> DP
        DP -- "plan + model trace" --> MOD
    end
    PY["py/ — starts the loop and measures it<br/>cold_regress.py (the regression) · verification<br/>fidelity diagnostics · calibration levels · MCP server"]
    PY -.-> loop
    loop --> OUT["plan file: one input=tick,buttons line per edge (a bitmask, bit 0 = jump)<br/>plus the model trace and the per-layer bands"]
```

One round trip per iteration: the mod hands the solver the level and, after a
replay that died, the game's own state to restart from; the solver hands back a
plan and the model trace the next divergence is measured against.

### 2.1 `dp/` — the solver core

* `dp/src/dp/*.hpp` is one translation unit split into modules in dependency
  order, each header including the previous one: the speed tables and per-mode
  constants, then geometry (hitboxes, OBB/SAT tests, rotated gameplay frames),
  then the world (moving geometry, triggers, level loading), then `step` — the
  per-tick physics step — with `fixup` and `cli` on top. `dp/src/leveldp_main.cpp`
  is a three-line `main` around `dp::cliMain`, and the mod calls that same
  function in-process: the CLI and the mod run one solver, not two.
* The level comes in as an objrects CSV through `loadLevelFrom(std::istream&)`.
  Setting `dp::g_levelCsv` makes it read a buffer instead of a file, which is
  how the mod solves a level it never wrote to disk.
* States are **exact** (double y / vy plus discrete flags). The per-layer hash
  only deduplicates; it never snaps a state, so every surviving path is a
  replayable plan. The DP is a candidate generator; the proof is always a plain
  replay in the game.
* A cell keeps **two** representatives, the highest and the lowest `vy` in it.
  First-wins was tried and kept the slowest lineage in every cell, which
  strangled climbs (a ship's climb rate collapsed to ~0.02 vy/tick); keeping one
  extreme instead discarded the dive-recovery lineages. Both extremes is what
  survives both.
* When several states reach the goal, the plan emitted is the one whose **route
  kept the most room**, not the one that happened to be enumerated first. Each
  state carries `tight`: how many ticks its lineage spent with less vertical
  clearance than the model's own error. It is carried rather than recomputed —
  clearance at the goal itself says nothing, because every state that gets there
  is in open sky — and it is deliberately *not* part of the dedupe key, being a
  property of how a state was reached rather than of the state. Ties keep the
  incumbent, so a level where nothing is tight emits exactly what the old rule
  did.
* Output contracts: `SOLVED at ...`, `PARTIAL: frontier died at t=.. x=..`,
  `FAILED: ...` on stdout; files `<out>` (the plan, `input=<tick>,<buttons>` lines,
  where `<buttons>` is a bitmask of the buttons held from that tick: bit 0 is
  player 1's jump and bit 1 is player 2's (a two-player level's second button,
  which the solver plans and this version replays; the harness will not launch
  such a plan on a package without the path), the other
  bits are reserved and a reader that does not drive one refuses the plan),
  `<out>.trace.csv` (per-tick model state) and `<out>.bands.txt` (per-layer
  frontier width).

### 2.2 `src/` — the Geode mod

* `src/mod/*.hpp` — the infrastructure, again a header chain: configuration
  (`autorun.cfg` keys), the trace/dump/result writers, hotkeys and the overlay,
  command polling, crash post-mortem, the stall watchdog, plan I/O and the
  session lifecycle.
* `src/mod/hooks_*.cpp` — one translation unit per hook group: system
  (achievement and statistic blocking, music, focus), menu, game layer (tick
  control, the fast loop, tracing), player (state dump) and play layer (attempt
  boundaries, checkpoints, death and completion).
* `src/solver/*.hpp` — level-data export, moving-geometry recording
  (grouptrace), the clearance table, the raw player snapshot, the section
  solver (§3.1), the coin prerequisites census (`route.hpp`) and the level
  slice (`slice.hpp`, with the session side in `src/mod/level_slice.hpp`).
* `src/mod/itermap.hpp` — what the repair loop's rounds cost, and where. Each
  round's death, each fixup and each veto is recorded as the loop makes it,
  filed as `itermap_lv<N>.txt` when the solve ends either way, and drawn back
  over the level — during a solve by default, in a replay when `F10` asks.
  Recording only: nothing in the loop
  reads it back. `py/itermap_from_log.py` rebuilds the same file from a run's
  log, so runs made before it existed are readable too. How to read what it
  draws: [ITERMAP.md](ITERMAP.md).
* Protocol, per data root: `autorun.cfg` (session config and plan),
  `plan_in.txt` / `cmd.txt` (the next plan, live commands), `result.txt`
  (session results, one line per event), `dump.csv` (per-tick player state),
  `grouptrace*.txt` (moving geometry), `itermap_lv<N>.txt` (the iteration map).
* `src/mod/dp_bridge.{hpp,cpp}` — the mod's side of the solver core. The mod
  does not compile the core: its first copy is the CLI's own compiled
  `gdsolver_dpcore` (`dp/src/dpcore.cpp`), and its second copy
  (`src/mod/dp_bridge2.cpp`, for the plain search beside the cap ladder) is a
  library of its own built with the same options. The bridge mentions neither
  Geode nor cocos; everything crosses the boundary as plain C++
  (`dp/src/dpcore.hpp`). `dpselftest=1` reports what the core made of the
  level, in the CLI's own words, so the two can be compared line for line.
* `src/mod/repair.hpp` — the solve loop itself (§3), in the game. cfg
  `dpsolve=1` builds the level from `PlayLayer`, solves it on a worker thread,
  replays the plan, and when the replay dies re-anchors on the player state it
  recorded tick by tick and solves the tail from there. The anchor record is
  read straight off `PlayerObject` as the replay runs, not parsed back out of
  `dump.csv` — being in-process is what makes that possible.
* Safety: while the mod is *driving* — solving or replaying — achievements,
  statistics, coins and the level's own record are all blocked; a human playing
  with the mod merely loaded records normally. The count of blocked events and
  a `level record changed:` line are printed at session end.

### 2.3 `py/` — tools

Nothing here solves. The loop lives in the mod; `py/` starts it and measures it.

* `cold_regress.py` — the cold regression: every level solved from nothing by
  the game's own loop, on a pool of isolated GD workers. Each run writes a
  manifest beside its logs (the commit, the package, every cfg value the mod
  held and the core's built-in defaults); `cold_manifest.py compare` refuses two
  runs that differ in anything but what was named, and `check` confirms a run
  against an expected set before it is blessed.
* `verify.py` / `verify_solutions.py` — replay every solution in the game;
  `quick_regress.py` — a fidelity regression that needs no worker at all;
  `fixcensus.py` / `fixfam.py` / `fidelity_diff.py` — where and why the model
  and the game disagree; `mklevel.py` + `calib_units.py` — calibration levels
  that measure one physics rule across all modes, and `test_*_rig.py` pinning
  particular rules against the game's own rows; `gdtas/` — worker
  provisioning, control and the shared readers; `mcp/` — an MCP server exposing
  the same protocol for interactive probing.

## 3. The solve loop (one level)

```mermaid
flowchart TD
    E["export the level from the game<br/>objects · triggers · groups · turned hitboxes"]
    E --> S["run the DP from the current anchor, bounded horizon,<br/>splicing the tail onto the prefix the game has verified"]
    S --> RP{"replay the plan in the game"}
    RP -- clears --> DONE["solution saved"]
    RP -- dies --> DIV["first tick where the game's dump and the<br/>model's trace disagree beyond tolerance"]
    DIV --> FX["record that one transition as a fixup:<br/>the model now applies the game's result<br/>whenever it meets that state again"]
    FX --> AN["new anchor: the game's real state some ticks<br/>before the death, off a ladder of depths"]
    AN --> S
```

The first pass runs from tick 0; every later one starts at an anchor and splices
onto the prefix the game has already verified. What the diagram leaves out are the
mechanisms for the cases where the model is *blind* rather than merely inaccurate:
dead bands, touch triggers that must be entered, a ladder of anchor depths,
phantom-death detection, and — where none of that gets the plan across a wall —
the section solver of §3.1, which searches the game itself there. All of those
the loop drives itself.

Three things decide how much each repair costs. **The plan length** (cfg
`dpstephorizon`): a replay the game ends within one step
(3,000 ticks) of its anchor says the model is wrong there, so the next solve plans
one step and lets the game check it; a replay that outlives its step, or a wall the
run stops at twice, gets a plan to the end of the level. **The rejoin** (always on;
dp's `--rejoinwatch` / `--rejoinuse`): the model's
trace of the plan that died is handed to the next search, and when a state that did
not pass through the death comes back onto that trace (same y, velocity, mode,
gravity and frame), the search stops there and the plan carries on with the old
plan's inputs. The game still flies all of it; the rejoin only skips solving again
what the search would have re-derived. On the 22-level suite in one session the
suite went from 30 minutes (v0.1.4) to 19; the rejoin alone took it from 1,437 s to
1,120 s. **The flight** (cfg `cpflight`, 600 by default): every flight takes a
checkpoint every 600 ticks, and a plan whose inputs agree with an earlier attempt's up
to one of those checkpoints is flown from it instead of from tick 0. What the loop
records per attempt (the anchor rows, the random seeds, the coin state, the
moving-geometry recording) is taken over from that attempt up to that tick. A clear
from a checkpoint is not filed: the plan is flown once more from tick 0, and that
flight's clear is. The game leaves some state of one attempt to the next, so a
checkpoint is used only when the attempt that took it inherited the same flight band
and rect latches as the attempt about to fly (cfg `cpcarryparts`); what else an
attempt inherits was measured not to change a flight. The restore also puts back
what the game's own checkpoint does not save: broken blocks, the players' follow
history and the rotation of rotated objects (cfgs `cpdisabled`, `cpfollowfloats`,
`cprotnode`). A level that keeps items across attempts flies from tick 0
(`cppersistguard`).

One more saves flights rather than solves. **The model re-anchor** (cfg
`dpmodelreanchor`, 3 by default): a plan whose own walk through the model already
dies is not flown as it is. The loop replays it through the solver from the same
anchor, has the model write its whole state a few ticks before that death (dp
`--dumpstate`), and searches again from that state (`--start-state`), the same
ladder of depths the game's anchors use, up to three times per repair. The file
carries what an anchor taken from the game's recording cannot: the rings and pads
already used, every trigger's fire state, the moving geometry's last placement. A
replay from it continues the uninterrupted replay bit for bit.

Every run is *cold*: no seeds, no previous solutions, no external inputs. The
fixups of a run live only in that run.

### 3.1 The section solver

A fixup is local: it applies where the model meets that exact state again, and
nowhere else. On a stretch the model is simply wrong about, the loop can grind —
every death teaches one fixup and the next plan dies a few ticks further on. When
the work spent at a wall reaches what a section solve would cost, or the ladder
runs out of anchors, the loop drops the model for that stretch: it replays its
deepest verified plan to a practice-mode checkpoint before the wall and searches
**the game itself** from there, breadth-first over the two inputs, the same shape
as the DP. A leaf that reaches the goal is replayed plainly from the checkpoint,
and only one the replay reproduces is spliced into the plan, which the game then
flies like any other, and files only once it has cleared from the start of the
level. That is affordable because a section makes the question narrow: the entry
is fixed, the exit is binary and the horizon is a few hundred ticks, so there is
no fitness function to choose.

[SECTION_SOLVE.md](SECTION_SOLVE.md) has when a rung starts and where its window
goes, the handoff, the search, the splice and its pin, what it costs and the kind
of death it cannot see.

### 3.2 Coins and heavy levels

With coins on, the collected set is part of the search state, and the loop can
also work out what a coin needs to have happened first — a key, a touch box, a
switch — and route through it (cfg `routeprereq`, on by default):
[COINS.md](COINS.md).

A level with at least 10,000 objects its run cannot depend on is solved on a copy
without them, and every plan that clears the copy is verified on the level itself
before anything is filed; a wrong cut costs rounds, never a wrong answer:
[LEVEL_SLICE.md](LEVEL_SLICE.md).

## 4. Running inside the game

A user needs only Geometry Dash, Geode and `gdsolver.solver.geode`:

* **`dp/` is linked into the mod.** The level model is built in memory from the
  loaded `PlayLayer` by the same code that writes the text exports, and a solve
  in the game produces a plan byte-identical to the CLI's.
* **`src/mod/repair.hpp` holds the loop** — solve → replay → learn → re-anchor →
  splice → replay — in the game, with no external process. All 22 official levels
  and the 17 spin-off levels solve cold this way (`py/cold_regress.py`).

Acceptance for a change is behavioural identity where identity is claimed: the
CLI must produce byte-identical plans and traces on a fixed suite
(`py/quick_regress.py`), and the loop must reproduce the cold-run fingerprint —
iteration, plan hash, death tick and x, fixup hash. The loop prints those itself
(cfg `dpfingerprint`). `py/cold_regress.py` judges a run on clearing, coins and
the record line, and prints each level's iteration count against the baseline,
which is adopted from a reviewed one-session run (`--adopt`); the `[fp]` lines are
compared with the baseline run's separately — the script stores the last one but
does not check it. The iteration count does not fail a run;
the wall clock is not deterministic and is never the criterion.

The two halves of that measure different things, and the section suite does not
predict the loop. It anchors on the game's real state every few hundred ticks
and asks how long the model tracks from there, so it sees physics and nothing
else. The loop's iteration count also carries which corridor the search walked,
and the search is deepest-first: one rule can close four sections and leave the
count untouched, or improve the physics and double it by sending the run down a
different route. Both have been measured on the same change. Read the section
suite as "did the physics move", never as "is this an improvement".

### 4.1 Defaults, and turning one off

The defaults are the configuration the release's cold runs passed with: every model
rule and loop behaviour those runs used is on without being asked for, and a player
has no switch for any of them. For an A/B arm each can still be turned off, in the
session config (`autorun.cfg`, or `py/cold_regress.py --cfg key=value`) or on the
solver's command line:

| area | on by default | turned off by |
|---|---|---|
| the model's rules (`leveldp`) | every measured ramp, dual, portal and kill rule. The 0.4.0 clean-up removed 276 switches, so the rules older than it mostly have no off arm (a few keep one); a rule added since keeps a `--no-` spelling until the rule is folded into the default and the switch removed (171 in v0.6.0, 53 in v0.7.0; dp/removed_flags.txt) | the older ones: `--no-portalpress`, `--no-portalunpin`, `--no-hazendpoint`, `--no-upsidecoyote`, `--no-padtable`, `--no-airpress`, `--no-anchorride`, `--no-rotsplit`; the newer: `--no-<flag>`, e.g. `--no-teleporttrig`, `--no-portreread`, `--no-flipbonk`, `--no-stopjump`, `--no-chanmove` |
| the search (`leveldp`) | `--ladderovercap`, `--ladderprefix`, `--rejoinprobe 5`, `--statecross`, `--crossbehind`, `--incanchor`, `--rlbound 1500` | `--no-ladderovercap`, `--no-ladderprefix`, `--rejoinprobe 0`, `--no-statecross`, `--no-crossbehind`, `--no-incanchor`, `--rlbound 0` |
| the repair loop | `cpflight=600`, `cpcarryparts=2`, `dplearnresets=1`, `dpmodelreanchor=3`, `dpcapenvrepeat=3`, `dpmissedportalrepeat=3`, `dpspeedfirst`, `camrand`, `vtime` (not given: the fast loop's dt while it runs), `offboardfree`, `offboardband`, `voidband`, `cpfollowsection`, `cpdisabled`, `cpfollowfloats`, `cprotnode`, `cppersistguard`, `dpseedpickups`, `snapnodesync`, `obbnolatch` | `cpflight=0`, `cpcarryparts=31`, `dplearnresets=-1`, `vtime=0`, the others `=0` |
| coins ([COINS.md](COINS.md)) | `dpcoinwin=60,2,700`, `coinapproachreach`, `coinmissearly`, `routeprereq` | `=0` |
| the section solver ([SECTION_SOLVE.md](SECTION_SOLVE.md)) | `dpsectierfirst`, `dpsecstate`, `dpsecrecaway`, `secsnapem`, `secjbkeep`, `secheaddash`, `secheadaccel`, `secgracedash`, `secrungcoinoff`, `secbookfirst`, `secredoreach`, `secwsnapauto`, `secwsnapgate`, `sectionhooklazy` | `=0` |
| the level slice ([LEVEL_SLICE.md](LEVEL_SLICE.md)) | `slicetriggers`, `sliceuids`, `slicekeepgroups`, `slicenotouch`, `slicefirstref`, `slicegiveupcopy` | `=0` |
| the seek bar ([RUNNING.md](RUNNING.md)) | `seekcp=600` (a checkpoint every 600 ticks of a replay), `seekblind` (a seek runs screen-off up to the nearest one) | `=0` |

A switch the clean-up removed is refused by name: `leveldp` stops with "<spelling> was
removed in <commit>; the behaviour is fixed (since <commit>)", and an `autorun.cfg` that
names a removed key (`dpladderback`, `dprotextend`, ...) makes the game refuse to solve,
with the same two commits in `result.txt`. Some keys are off by default, because they
are diagnostics or change what is being solved: `seccpfallback=1` lets the loop's
section searches run on checkpoints again (see
[SECTION_SOLVE.md](SECTION_SOLVE.md#4-the-search)); `attemptone`, `attemptcount=k` and
`persistworld=k` pin what an attempt-dependent level reads
([CUSTOM_LEVELS.md](CUSTOM_LEVELS.md)); `takeover=<tick>` hands the plan to a person
([TAKEOVER.md](TAKEOVER.md)); `dpsearch=beam|montecarlo` swaps the search; and `areaenv=1`
treats where the game's random numbers can put an Area Move's objects as deadly (on
until v0.5.0; the seeds are fixed while the mod drives, so the recording holds this
game's positions). What each key does, and what it was measured on, is in its comment
in `src/mod/config.hpp`, `src/solver/secsolve.hpp` or `src/solver/psnap.hpp`.

## 5. What the model covers

The DP's rules are measured one at a time against what the official levels
actually use — calibration rigs, traces out of the game, the disassembly. That
measuring is also the boundary of what is supported.

* **Custom levels** go through the same path: level selection is the game's own
  and the model is built from whatever `PlayLayer` loaded, so many of them solve
  as they are. Nothing keeps them honest, though. An object, a trigger or a mode
  the model has never met is a disagreement the loop can only discover by dying
  in a replay, and it repairs one disagreement per iteration — so an unmodelled
  level does not fail, it crawls, or clears where the section solver finds a way
  through the game itself. The corpus the boundary was measured on is
  almost entirely 1.x, so a level built from early-era parts has the better
  chance — but the predictor is the mechanics a level uses, not its date, and
  the two only correlate because mechanics arrived with versions. A supported
  subset has to be defined in objects and modes; a version is a first guess at
  which of them a level contains. What the surveys found, gimmick by gimmick:
  [CUSTOM_LEVELS.md](CUSTOM_LEVELS.md).
* **The objective is survival, or survival and every coin**: alive at the end of
  the level, and with the play menu's Coins switch (cfg `coinroute`, dp `--coins`)
  also every coin in it. With coins on, the collected set is part of the search
  state, a coin is collected when the player's own rect overlaps it where its
  group has put it, and a state that leaves an uncollected coin behind is dead;
  the search is not steered towards a coin, the goal is simply not reached
  without it. The loop ends a replay that passes a coin GD did not credit and
  repairs it from there. All 22 official levels clear with 3/3 that way, and so
  do the six spin-off levels that have coins; without coins, a plan that collects
  one of the official 66 did so by accident — 23 of them with the v0.3.0
  solutions, by the game's own count. Where the coins are is always exported, and `coinMode` turns on two witnesses
  that both work while every award is blocked: the mod's own pickup test (a
  distance from the coin's loaded position) and GD's own, hooked at
  `GJBaseGameLayer::pickupItem`, which is where `collisionCheckObjects` credits a
  coin. They are reported side by side (`coin:` / `coingd:` / `coincmp:`) because
  the first is a claim about the second, and comparing them is what says whether
  a route that plans to collect a coin would actually be paid for it. The
  measurement on the v0.3.0 solutions: the same verdict on 18 of the 23 coins
  GD credited on the coins-off ones and on 50 of 66 on the coin ones, never a
  pickup GD did not credit, and the rest real pickups it missed — see COIN_RADIUS in
  `src/solver/solver.hpp`.
  `itemcnt:` reports GD's item counters, which is what actually gates the coins
  that have to be made to appear (lv21's third and lv22's third). How each layer
  treats coins, and how the loop finds what a coin needs first:
  [COINS.md](COINS.md).
