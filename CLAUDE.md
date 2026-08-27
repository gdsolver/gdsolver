# gdsolver — working guide

An automatic TAS solver for Geometry Dash. A reachability DP (`dp/`) proposes
input plans; the game itself, driven by a Geode mod (`src/`), verifies them and
supplies the real state whenever the model is wrong. See
[docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) for how the pieces fit.

## Layout

```
dp/       solver core (header chain in dp/src/dp/) + the leveldp CLI
src/      Geode mod: mod/ (infrastructure + one TU per hook group),
          solver/ (level export, moving-geometry recording, psnap, secsolve)
py/       cold regression (cold_regress.py), verification, fidelity
          diagnostics, calibration rigs, worker tooling. Nothing here solves:
          the loop is src/mod/repair.hpp, and py/ starts and measures it
mcp/      MCP server: ask the running game questions interactively
data/     solutions (tracked) and per-run working files (not tracked)
data/rigs calibration levels (.lvl / .units.json / .plan.txt)
tools/    repo utilities (e.g. check_code_only_diff.py)
```

Machine-local, never published: the private lab tree (`GDSOLVER_LAB`, default
`../GD-lab`) holds the official-level dumps, `gdref`, the `fixups_log`
archives, the development notes and the one-off analysis scripts. Paths come
from `py/gdtas/paths.py`; nothing should hard-code an absolute path.

## Build

```powershell
cmake -S . -B build-dp -DGDSOLVER_BUILD_MOD=OFF   # solver CLI only
cmake --build build-dp --config Release
geode build                                        # mod + CLI
python py/dev.py                                   # build, deploy, launch GD
```

## Rules that are not negotiable

1. **Solving is always cold.** No seeds, no previous solutions, no external
   ground truth. The only exception is an explicit resume of the same lineage,
   and a result reached that way is never reported as "solved cold". A run's
   fixups live only in that run.
2. **Ask the game, do not guess.** Object behaviour, hitboxes and constants are
   measured — through the MCP tools (`gd_*`), the calibration rigs, or the
   disassembly — before the model is changed. A rule justified by a single
   coincidence usually breaks 47 cases that were right.
3. **Behaviour-preserving changes must be proven, not asserted.** The acceptance
   criteria: byte-identical solver output on the replay/cold suite (`python
   py/quick_regress.py`, a couple of minutes, no worker), and for a change that
   reaches the loop, `python py/cold_regress.py` — same iteration count per level as
   `data/cold_baseline.json`, and the `[fp]` lines the loop prints. **Check
   which binary you are measuring**: `quick_regress` drives
   `build/dp/RelWithDebInfo/leveldp.exe`, which only `geode build` refreshes,
   so a change compiled into `build-dp` alone is measured against a stale exe
   (that mistake blessed eleven red levels as green on 2026-08-26). A
   comment-only edit is checked with `python tools/check_code_only_diff.py
   <file>`; after touching imports, paths or module-level constants run
   `python tools/import_smoke.py` (two seconds — a missing import otherwise
   shows up an hour into a run).
4. **Measure the instrument before believing it.** Buffered logs, stale dumps,
   a second process writing the same data root, an isolated data dir carrying
   yesterday's files — these have each produced convincing wrong conclusions.
   One cold run at a time, in its own `--data-dir`.
5. **Public tree is English.** Code comments and documentation here are in
   English; the private lab may be in Japanese.

## Safety (kept from the project spec)

While the mod is *driving* — solving or replaying — nothing is recorded:
achievements, statistics, coins, and the level's own record (percentage, best,
attempts, jumps, orbs, diamonds, keys). Playing yourself with the mod merely
loaded records normally; the gate is `botDriving()` (config.hpp), which asks
whether an automated session is open rather than which mode is running — gating
on a list of modes is what leaked the last time. Every session prints what it
blocked and a `level record changed:` line that must read `none`. Replays are
marked as bot input; nothing here may be used to claim a record.

`CLAUDE.local.md` (not tracked) holds the original Japanese spec and the
machine-specific operating notes.
