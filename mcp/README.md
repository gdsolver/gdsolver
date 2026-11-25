# gdmcp — an MCP server that drives GD for analysis

A separate line from the main one (the solver). Its only purpose is **to make the
analysis round trip fast**; the search itself stays inside the MOD.

## Why it was built

The control surface (`servemode` / `cmd.txt` / `plan_in.txt`) had existed for a while.
What was missing was **the ability to keep a resident worker alive**:
PowerShell tools lose their state between calls, so the handle to the resident session
started with `Start-Serve` was lost every time, and every single observation paid for
a full GD launch.
An MCP server is a long-lived process, so it can hold the same GD and query it as many
times as it likes.

Measured (lv1, worker-98):

| Operation | Measured |
|---|---|
| Session start (cold) | 1.8 s (up to `session_start`) |
| Supplying one plan through to the result | 0.6 – 1.2 s |
| Including the automatic reopen after a run to completion | 4.6 s |
| 5 plans + an object query | 16.4 s in total |

## Scope of phase 1 (current state)

**Not a single line of C++ is touched.** It only wraps the existing cfg / commands /
dumps, and therefore does not conflict with the main line's build.

| tool | what it does |
|---|---|
| `gd_session_open(level, worker_id=100, cfg)` | launch one worker and hold it; on an open session with the same `.geode` and cfg, switch levels in the running game (`fresh=True` forces a launch) |
| `gd_session_close()` | release it |
| `gd_session_info()` | what is held (level / sha1 of the .geode in use / last result) |
| `gd_run(plan \| plan_file)` | run one plan and return **only a summary**; the level is entered afresh first if it already ran something (`in_place=True` resets it where it stands instead) |
| `gd_death_context(back_ticks, x_pad)` | pre-death state + events + hitboxes + **suspects** |
| `gd_diff(trace_file, t0, t1, tol, limit)` | **the first tick** where model and GD diverge |
| `gd_trace(t0, t1, cols, stride, limit)` | per-tick state, by range / columns / stride |
| `gd_events(t0, t1, kinds, limit)` | the events in trace.csv |
| `gd_objects(x0, x1, ... \| tick, pad)` | objects in a range with GD-measured hitboxes |
| `gd_cmd(cmd)` | the escape hatch for `status` / `diag` / `pause` / `resume` / `step N` |
| `gd_reload()` | reload worker.py / data.py (tools.py needs a restart) |

## Fewer relaunches (2026-10-27, 22091)

A session used to be relaunched after every plan that ran to the end (4.6 s), and every new level
was a relaunch. Both are gone; what replaced them is accepted on one criterion: **a result obtained
without relaunching must equal the result of a relaunch, row for row** (`python -m gdmcp.equiv`).

What changed (the MOD is touched after all, so the "no C++" promise below is the phase-1 one):

- `serverearm=1` (the tools set it): the worker stops at a clear as it does at a death and keeps the
  level. GD's own completion (results screen, music) is not run for it.
- `cmd switch`: leave the level and enter the one `autorun.cfg` names, through the same exit and
  entry a `levels=` suite uses between two levels (`endSession` -> `onQuit` -> `SuiteKeeper`). It is
  the path the `--one-session` cold regression already exercises, so a level started this way is
  configured by the code that configures every level after the first there.
- `gd_session_open` on a session that is already open on that worker, with the same `.geode` and the
  same cfg, **switches** (`switched: true`) instead of relaunching. `fresh=True` forces a launch.
- `gd_run` on a level that has already run something **enters the level again first** (~2 s,
  `entered: "re-entered"`). `in_place=True` resets where it stands instead (~0.5 s).

Why re-enter and not just reset: GD's reset leaves some state as the last run had it, and that is
visible. Measured (`gdmcp.equiv`, arms: F = a launch per plan, R = reset in place, S = the level
entered afresh before every plan, T = these tools with their defaults; cases per level: the stored
clear, a plan cut short so that it dies, the clear again; compared: every row of `dump.csv` and
`trace.csv` but `frame`/`attempt`, `grouptrace_last.txt`, the `death:`/`complete:`/`gt_last:` lines):

| arm | official 1-22 (66 cases) | 7 customs with moving geometry and triggers (21 cases) |
|---|---|---|
| R reset in place | 64 equal | 13 equal |
| S level entered afresh | **66 equal** | **21 equal** |
| T the tools, defaults | **66 equal** | **21 equal** |

In R the physics columns agree nearly everywhere, but not always: the moving objects' cached rects at
the first ticks differ (`grouptrace_last.txt` of lv21 and four customs), `pmin`/`pmax`/`snapdist`
hold the previous run's value until their first write (up to 9,700 ticks on lv2), and on one custom
`y` differs by 2.5e-4 for 103 ticks. S and T show none of it, not even those stale columns. The
plain reset after a death was always like R; it is only now that it is written down. Compare the
dump of an `in_place` run with the dump of a relaunch only through the physics columns.

**Until the reset leaves nothing behind, a reset in place (`in_place=True`, arm R) is an optional
argument of `gd_run` and nothing more: do not put it on the default path of a suite, a batch or the
solver.** Those enter a level afresh (`switch` / `levelcfgdir`), which is what was measured equal.

Two things the comparison does NOT count, on purpose: the frame-cut events (`BGL_update_pre/post`)
of `trace.csv`, because the worker that stopped at a clear is still locked by the end sequence and
runs one tick in its first frame, so the same ticks are batched differently; and the lines the two
arms print differently by construction (GD's own end screen, `endtrigger:` which fires one tick after
`complete:`, `clearcheck:` which the mod prints for the first 40 clears of a *process*).
In the first T pass over the customs one case (lv139771614, first plan) had two extra
`handleButton_BLOCKED` rows in `trace.csv` (a real button event reaching the worker window and
blocked; the dump was equal); the same case was equal in a rerun alone and in a full second pass.
Measured on w100/w102 (22091), per operation:

| Operation | Measured |
|---|---|
| Launch GD to the first plan (`gd_session_open`, cold) | 3.1 - 3.9 s |
| Level switch in the running game (lv1-22 and customs of up to 20 MB) | 1.6 - 2.1 s |
| Next plan after a clear, before | a relaunch: 3.1 - 3.9 s + the plan (4.6 s with the old reopen) |
| Next plan after a clear, now, `in_place=True` | 0 s + 0.4 - 0.6 s for a run that dies near the start |
| Next plan after anything, now, default (re-enter) | 1.7 - 2.1 s + the plan |
| 66 official cases through T (22 levels, 22 switches in one game) | no relaunch after the first |

Not covered: calibration rigs (`levelfile=`) cannot be switched to and still relaunch; a change of
cfg between two opens relaunches (a key the new cfg leaves out would otherwise survive from the
old one, the mod parses cfg keys into globals that no reset puts back).

## A cfg per level in one game (batch tools)

`cold_regress.py --one-session --levels 16 17 18 --level-cfg cfgs.json` (cfgs.json is
`{"17": ["trigtrace=1"], ...}`) solves every level in one game, each with its own cfg on top of
`--cfg`: the mod reads the level's whole cfg when it enters it (cfg `levelcfgdir`; a level with no file
reads autorun.cfg again). The cfg keys whose variable is a global of its own are put back to their
pre-cfg values first (a generated snapshot, `tools/gen_effective_cfg.py`) -- without it
`trigtrace=1` of one level stayed on for the next three, which `py/cfg_swap_check.py` caught as a
`cfgeff:` difference. That script is the proof to rerun after touching the parsers: one launch per
level against one game, per level the whole `cfgeff:` line (416 keys), the last `[fp]` and the record
verdict must be identical. Measured on lv16-21 with five cfg variants: 6 of 6 equal. (The
"(restored N)" count after the record verdict is one higher for every level after the first in a
game, as in any `--one-session`; it is printed, not compared.)

## Sweeps and the memory of a resident game

`python -m gdmcp.sweep` runs a family of plans on one resident game and prints a table (a CSV
with `--csv`): one press moved by -3..+3 ticks, every press from a tick on, an x / y / vy
injection grid, or a list of plan files.

```powershell
$env:PYTHONPATH="$PWD\mcp;$PWD\py"
python -m gdmcp.sweep --level 1 --worker 100 --plan data/solution_lv1_dp.txt press --index 11 --by=-3:3
python -m gdmcp.sweep --level 1 --plan P inject --tick 380 --y 100:112:7 --vy 0:12:5 --after 30
python -m gdmcp.sweep --level 16 plans a.txt b.txt
```

Each run enters the level afresh (the exact mode); `--fast` resets in place instead.

`python -m gdmcp.soak` repeats one short run n times and writes the GD process's private bytes and
working set to a CSV (every 5 s and after every run), to see whether a resident game keeps growing.

Measured (22091, Windows, the run is the first 3 inputs, which die near the start; private bytes,
MB, after 0 / 10 / 50 / last run):

| soak | runs | 0 | 10 | 50 | last | slope, 2nd half |
|---|---|---|---|---|---|---|
| lv1, re-enter before each run | 300 | 757 | 819 | 821 | 837 | +3 KB/run |
| lv1, reset in place | 300 | 760 | 812 | 811 | 800 | -71 KB/run |
| lv1 / lv2 in turn (a switch between every two runs) | 262 | 792 | 825 | 829 | 829 | -14 KB/run |
| custom level A, re-enter | 150 | 771 | 906 | 914 | 891 | +76 KB/run |
| custom level A, reset in place | 150 | 766 | 891 | 890 | 890 | -5 KB/run |
| custom level B (11 MB), re-enter | 80 | 767 | 1117 | 1109 | 1112 | noise |

It does not grow: the first ~10 runs add 50 MB (lv1) to 350 MB (the 11 MB custom) and then the
number stays within +-30 MB for as long as it was run (the working set likewise: 270-570 MB).
Entering a level again and switching between levels cost nothing that accumulates, and neither does
the reset in place. So **no periodic restart is needed for memory.** What this did not measure: a
run that flies a whole level in each iteration for hours, and the solver's own DP memory (that is
the batch runner's, which stops below 3 GB free). The tools keep one safety net, `MAX_PRIVATE_MB`
(4096, three times the largest value seen): a game past it is relaunched before the next run
(`entered: "relaunched (memory)"`).

## Phase 2 (2026-07-31, with additions on the MOD side)

**State injection and cut-off.** These exist to remove the most expensive thing in
analysis: "first prepare a plan that reaches the state you want to investigate". They
are used as arguments to `gd_run`.

```
gd_run(plan=[], inject=[{"tick": 380, "y": 105, "vy": 11.18}], stop_at=390)
  -> {"outcome": "stopped", "tick": 390, "x": 505.02,
      "state": {"y": "127.482", "yvel": "9.020", "mode": "cube", ...},
      "injected": [{"t": "380", "x": "492.036", "y": "105.000", "vy": "11.180"}]}
```

The MOD side interprets 2 lines that piggyback on `plan_in.txt` (`loadPlanExtras`):

    inject=<tick>[,x=<v>][,y=<v>][,vy=<v>]
    stopat=<tick>

**Why plan_in.txt and not cmd.txt**: cmd.txt is polled at ~5Hz, so it cannot name a
physics tick (`fastloops` advances several thousand ticks in a single frame). Putting
them on the same path as the plan lets them be specified with the same precision as
plan inputs. `loadInputsFile` ignores anything that is not `input=`, so compatibility
with existing files is not broken either.

Constraints and practice:

- It takes effect from the T+1 transition (the injection happens **after** the physics
  update of tick T)
- Only **x / y / vy** can be injected. Mode, size and gravity carry too much attendant
  state to stay consistent under a raw assignment
- Put `stop_at` **before** the death. Behind it, the run dies first and it never fires
- Injections that fired appear in `injected`. If one is not there, it was discarded
  because the run stepped past the tick (a `warning` is attached if the counts do not
  match)

Not implemented (phase 3 onwards): screenshots with a hitbox overlay, death
disabled, divergence detection against the approximate model (`gd_diverge`).

## Phase 3 (2026-07-31, what turned out to be missing in actual use)

While solving lv11/lv12 there were three operations **for which the same python kept
being rewritten outside the MCP**. Those became tools.

- **`gd_diff`** — matches the model's `.trace.csv` against the GD dump by tick and
  returns a few lines from the first tick that diverged. This one job alone had been
  written into the scratchpad about 15 times. `tol` defaults to 0.3 (a divergence that
  means something), and lowering it to 0.001 makes even float rounding visible. When a
  dump contains several attempts it automatically picks **the longest attempt**
  (reading all of them naively makes the same tick overlap).
  The verdict even spells out how to read it: if they still match yet only GD finishes
  first, that is not a divergence — suspect **a kill test the model is not applying**
- **`suspects` in `gd_death_context`** — returning only the nearby objects meant that
  "for a circle of radius 21.6, the nearest point of the box is..." still had to be
  worked out by hand here every time. It now reads the player size (vsize), computes
  the clearance, and sorts by **deepest overlap first**. The tool also absorbs the fact
  that the formula differs between circles (saw blades) and rectangles
- **narrowed down the events of `gd_death_context`** — all kinds used to be emitted by
  default, so `PO_update_pre` / `checkCollisions` / `processCommands` lined up 3 rows
  per tick, and out of 302 rows the only meaningful one was a single `destroyPlayer`.
  Only the kinds relevant to the cause of death are kept now (when you want to see
  everything, call `gd_events` with no kinds)
- **`gd_objects(tick=...)`** — no more computing x by hand from `x ~ 1.29825*t`

`gd_diff` can also be used from the CLI (no session needed, directly on a local dump):

```powershell
python -m gdmcp.cli diff --trace data/solution_lv12_dp.txt.trace.csv `
                        --dump  data/solution_lv12_dp.txt.best.dump --tol 0.001
```

## `gd_call`: ask GD's own function (2026-10-29)

The trigger engine in `dp/` is a copy of GD's code. `gd_call` asks GD itself: it runs **one** GD function on a
**scratch** object with the input bytes you give and returns the object's bytes after the call. A port is
then checked against GD word for word, on inputs no replay happens to reach.

| | |
|---|---|
| `gd_call(function, patches, args, read)` | one call; returns `ok`, `sizes`, `player_in_hex` (the bytes GD was entered with), `player_hex` (what it left), `slope_in_hex` / `slope_hex` for the slope functions, and `read` (the values you asked for, decoded) |
| `python -m gdmcp.sweep ... call --spec spec.json` | the same over a grid: cartesian product of the `grid` axes, one call each, table + `--csv` |
| `tools/call_compare.cpp` | the port's answer next to GD's (`call_compare updateSlopeRotation in.hex out.hex dt`): `status: IDENTICAL` / `DIFFERS` with the words that differ |

Functions allowed so far: `updateSlopeRotation(dt)`, `collidedWithSlopeInternal(dt, slope, forced)`,
`collidedWithObjectInternal(dt, object, rect, skipCheck)` (`args`: `dt`, `forced` = skipCheck, `rect` = [x, y, w, h], `noobj` = 1 for a null object so the rect alone decides; the
returned `ret` is its bool). A function is
added to `kAllowed` in `src/mod/callverify.hpp` only after its decompile has been read for writes through the layer
pointer and the singleton `PlayLayer`.

**Usage.** The mod must run with `callverify=1` (cfg, off by default), and a level must be *held*: the mod reads
commands only while a level is open, so start one with `gd_run(plan=[[0, 0]], stop_at=60)` first.

```python
gd_session_open(1, cfg={"callverify": "1"})
gd_run(plan=[[0, 0]], stop_at=60)
r = gd_call("updateSlopeRotation",
            patches=[{"who": "p", "offset": 0x938, "type": "f64", "value": 0.5},   # the slope angle
                     {"who": "p", "offset": 0x38,  "type": "f32", "value": 0.0},   # rotation
                     {"who": "p", "offset": 0x3c,  "type": "f32", "value": 0.0}],
            args={"dt": 0.25},
            read=[{"who": "p", "offset": 0x38, "type": "f32"}])
# r["read"]            == {"p+0x38": 1.128010630607605}
# r["player_in_hex"]   bytes 0x38.. = 00000000 00000000 ; 0x938.. = 000000000000e03f (0.5)
# r["player_hex"]      bytes 0x38.. = a762903f a762903f ; 0x938.. = 000000000000e03f
```

`player_in_hex` / `player_hex` are the first 3152 bytes (`sizeof(PlayerObject)`) as hex, 2 characters a byte. Patch
types: `f32`, `f64`, `u8`, `i8`, `u16`, `i16`, `u32`, `i32`, `u64`, `i64`, or `hex` (raw bytes).

Cautions:

- **Every call is a fresh object.** A new `PlayerObject` (and slope `GameObject`) is made per call from
  `PlayerObject::create` / `createWithKey`, patched, called once and released. Nothing one call leaves is read
  by the next (an in-place reset kept the previous values once). Anything you do not patch has the value the
  constructor gives, not the value of the running player.
- **The scratch player has no game layer** (`+0xc28` is cleared after the patches) and sits on a scratch `CCLayer`
  that is on no scene, so the functions that guard their layer calls (`m_gameLayer != 0`) touch neither the
  real player nor the level. The real player and level are never read or written by a call.
- **Patch plain data only** (floats, doubles, flags). The pointer slots of the scratch object are its own heap
  pointers; `player_hex` words that hold them will not match a port. Compare with `call_compare`, which looks
  only at the physics words. **The mod refuses a patch that changes a pointer slot** (any 8-aligned word of the freshly made scratch
  object that holds a value above 32 bits and below 2^48, the vtable at +0 included; a value written elsewhere is judged by where it points: an address of committed memory (VirtualQuery) put into a plain word is refused too, a flag word that merely looks like one is not): `ok 0 refused: the patches change the word at p+0x..`,
  the function is not called, and the answer lists the slots (`ptr_slots_p`, `ptr_slots_s`). Copying a byte image from the running game over the object
  is exactly what this stops: it laid the game's sprite pointers into the scratch player and crashed worker 102 in `PlayerObject::setPosition` (2026-10-29). Skip those
  words when you seed a player from a recorded image.
- **The mod re-checks, after your patches, what could reach the real PlayLayer** (a caller's patch cannot undo it): `collidedWithSlopeInternal` is refused
  (`ok 0`, the function is not called) when the slope object has `m_slopeIsHazard` set (`slope_hazard_off` in the answer is where that byte lives); for
  `collidedWithObjectInternal` the mod sets the scratch player's `+0x7ed` to 1 after the patches (with 0 the function's hazard branch calls the singleton
  PlayLayer's destroyPlayer; with 1 it only sets `+0x98c`), so `player_in_hex` shows 1 there whatever was patched, and a hazard contact shows as `+0x98c`, not as a death.
- Only the functions in the allow-list can be named; anything else is refused with `function not in the allowed list`.
- A window with nothing compared is **UNOBSERVED**, never "identical" (`call_compare` exits 3 for it).
- Do not leave `callverify=1` in a cold or release configuration: it is a measurement switch.

Measured: `updateSlopeRotation`, port (`upd::updateSlopeRotation`) against GD, 224 inputs (8 modes x gravity
up/down x 7 slope angles x 2 start rotations): 224 identical.
## The promises that keep this from colliding with the main line

1. **Workers 98 and above only, and not under a running GD.** A session starts on worker 100, the
   second of the default pool (99-101, the workers of the game's current build); the batches
   (`cold_regress` and the like) start from 99. If 2 GDs sit on the same worker, result.txt gets
   mixed up and returns plausible numbers that belong to a different run, so a launch is refused
   while a GD runs under the worker: name a free one (`worker_id=`) when a batch holds 100.
   The code rejects `worker_id < 98` as well.
2. **Never write to the repository's own `data/`.** All writes go to the worker's data_root.
   Reads do go there (`objrects_lv<N>.txt` is invariant per level, so it is reused).
3. **The `.geode` is snapshotted and pinned at session start** (`mcp/.cache/`).
   The binary being measured is not swapped out even if the main line rebuilds. Which
   build a measurement was taken on can be stated with `gd_session_info().mod_sha1`.
4. **Do not touch C++** (for the duration of phase 1).

## Relationship to the absolute rule on solving

This server is **a tool for observation, not a tool for solving**.

- `gd_run(plan_file=...)` can replay a known solution, but that is only for
  "reproducing a known run in order to watch it".
- A known solution, or a truncation of one, **must never be used as a seed to solve
  from** (CLAUDE.md, "solving is always done from scratch").
- The same holds after state injection was added in phase 2. A state built by
  injection is not necessarily "a state reachable from the start", so feed what you
  learn there back into the model and **always verify the solution with a cold run**.

## Usage

Create the worker once, on the first run (~70MB each; Resources is a junction to the
frozen GD build that `python -m gdtas.gdbase freeze` makes, never to the Steam install):

```powershell
$env:PYTHONPATH="$PWD\mcp;$PWD\py"    # from the repository root
python -m gdtas.provision --worker-id 98 --min-worker-id 98
```

Registration as an MCP server lives in the repository's `.mcp.json` (see
`.mcp.json.example`). **It takes effect when Claude
Code is restarted** (MCP servers are only loaded at the start of a session).

When you only want to verify something without going through MCP:

```powershell
python -m gdmcp.cli smoke --level 1 --plan-file data/solution_lv1_final.txt
python -m gdmcp.cli objects --level 1 --x0 500 --x1 800
```

## Traps we hit (to stop them recurring)

- **(Fixed 2026-10-27 for sessions opened with `serverearm=1`, which the tools do by default: see
  "Fewer relaunches". Still true for a cfg with `serverearm=0`.) A run to completion kills the session.** The MOD's `pollCommandFile` sits inside
  `g_started && !g_sessionOver`, so a worker that has run to completion never reads
  cmd.txt again. Every subsequent supply becomes a 120 s timeout.
  → If `serve: loaded` does not appear within 20 s, reopen (measured recovery: 4.6 s).
- **Never put a BOM on plan_in.txt.** The MOD's `input=` test fails on the first line
  alone and the first input silently disappears. In the log it still looks normal,
  saying "loaded N inputs".
- **Read plan files with `utf-8-sig`.** The **reader side** of the same trap. Solution
  files written by PowerShell carry a BOM, so reading them as `utf-8` turns the first
  line into `﻿input=...`, it falls out of the regular expression, and the first
  input disappears. 2462 lines simply become 2461 inputs with no exception raised,
  and the symptom only ever shows up as "only the first jump never fires".
  This is exactly how "the MOD is dropping inputs" got misdiagnosed (2026-07-31).
  → `parse_plan_file` raises if a line contains `input=` but cannot be parsed.
- **PowerShell 5.1 reads a BOM-less UTF-8 .ps1 as ANSI.**
  A .ps1 with non-ASCII text (comments in a language other than English, say) must be
  saved with a BOM (without one it is a syntax error).
- **If an observation looks like "the input is not taking effect", suspect your own
  reading and writing first.** In the BOM case above, GD was run 13 times, varying
  lengths, ticks and history, before it finally came down to our own parser.
  **Emitting the same log on both sides and comparing them** comes first (in this case
  "the MOD says loaded 2461" against "the file has 2462 lines" settled it instantly).
