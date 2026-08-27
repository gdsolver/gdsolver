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
| `gd_session_open(level, worker_id=98, cfg)` | launch one worker and hold it |
| `gd_session_close()` | release it |
| `gd_session_info()` | what is held (level / sha1 of the .geode in use / last result) |
| `gd_run(plan \| plan_file)` | run one plan and return **only a summary** |
| `gd_death_context(back_ticks, x_pad)` | pre-death state + events + hitboxes + **suspects** |
| `gd_diff(trace_file, t0, t1, tol, limit)` | **the first tick** where model and GD diverge |
| `gd_trace(t0, t1, cols, stride, limit)` | per-tick state, by range / columns / stride |
| `gd_events(t0, t1, kinds, limit)` | the events in trace.csv |
| `gd_objects(x0, x1, ... \| tick, pad)` | objects in a range with GD-measured hitboxes |
| `gd_cmd(cmd)` | the escape hatch for `status` / `diag` / `pause` / `resume` / `step N` |
| `gd_reload()` | reload worker.py / data.py (tools.py needs a restart) |

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

## The promises that keep this from colliding with the main line

1. **Workers 98 and above only.** 90–97 are the Pool for the main line's batches
   (`cold_regress` and the like). If 2 GDs sit on the same worker, result.txt gets
   mixed up and returns plausible numbers that belong to a different run.
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
original GD):

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

- **A run to completion kills the session.** The MOD's `pollCommandFile` sits inside
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
  A .ps1 with Japanese comments must be saved with a BOM (without one it is a syntax
  error).
- **If an observation looks like "the input is not taking effect", suspect your own
  reading and writing first.** In the BOM case above, GD was run 13 times, varying
  lengths, ticks and history, before it finally came down to our own parser.
  **Emitting the same log on both sides and comparing them** comes first (in this case
  "the MOD says loaded 2461" against "the file has 2462 lines" settled it instantly).
