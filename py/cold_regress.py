#!/usr/bin/env python
"""The cold regression: every level solved from nothing by the game itself.

    python py/cold_regress.py                    # all 22, over the default pool (99-101), a launch per level
    python py/cold_regress.py --levels 16 22     # just those two
    python py/cold_regress.py --one-session      # all of them in ONE game
    python py/cold_regress.py --bless            # adopt this run as the baseline

`dpsolve=1`, no plan, no solution file, no seed: the mod builds the level out of
PlayLayer and runs the whole repair loop itself. This is the successor to
dp_solve_batch.py, which drove the same loop from Python; the levels are what is
being measured, not the driver, so what changed is only who holds the loop.

TWO ARRANGEMENTS, and the difference between them is itself a test.

  default        one GD launch per level, four in parallel. What to run while
                 working: it is the faster of the two and it isolates crashes.
  --one-session  every level in ONE game, in order, on one worker (`levels=` in
                 autorun.cfg; the mod's own suite:: carries it from one level to
                 the next). Slower -- serial, and roughly the same wall clock as
                 the parallel sweep because a solo level runs about twice as fast
                 as one of four -- and it is the ONLY arrangement in which the
                 mod has to clean up after a level.

Under the default arrangement every global starts at its declared value and the
launcher has emptied the data dir, so "starting a level leaves nothing of the
last one behind" is an assumption nothing can falsify. It was false for months:
a level's moving-geometry recording was still being offered to the next level
(src/solver/grouptrace.hpp), and all 22 stayed green because no two levels ever
shared a process. --bless therefore runs --one-session, so the numbers that
become the baseline are numbers the arrangement can disprove.

The acceptance is that the two arrangements AGREE, level for level: the
iteration count is a property of the level and the build, and if solving lv19
first changes lv18's, that difference is a bug and not a baseline.

Read the result as three numbers per level, in this order of authority:

  CLEARED     only `dpsolve: solution saved` proves it. A `complete:` line with
              pct=100 is ALSO what the nodeath recording pass writes -- it runs
              the level with dying switched off and the mod refuses it as "not a
              clear" -- and reading the percentage alone once reported lv22 as
              CLEARED on a run that gave up at 41%.
  iters       the iteration count. It is deterministic: two cold lv22 runs on
              the same build both took exactly 60, lv16 both took 102, lv18 both
              took 11. That is what makes this a regression rather than a demo,
              and it is the number the baseline compares.
  record      must read `none`. The safety gate (config.hpp botDriving) blocks
              the level's own record while the bot drives; a run that shows
              anything else here is a bug in the gate, not a solved level.

Wall-clock is reported but NEVER compared: with four workers each running its
own DP threads it inflates by roughly 2x against a solo run (measured, lv22:
465 s alone, 928 s in a full sweep) while the iteration count is unchanged.
"""

from __future__ import annotations

import argparse
import json
import re
import shutil
import sys
import threading
import time
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))

import cold_manifest
import run_guard
from gdtas.paths import BUILD_MOD, DATA, DEFAULT_POOL
from gdtas.worker import WorkerError, run_session, snapshot_mod

# The runner's own limits inside a launch (py/run_guard.py). main() and other runners that
# call one() / one_session() set these; 0 turns a limit off. LEVEL_DEADLINE_S is the
# seconds one level may hold the game; MEM_FLOOR_MB the available physical memory under
# which the arm is ended.
GUARD = {"level_deadline_s": 0.0, "mem_floor_mb": 0.0}

BASELINE = DATA / "cold_baseline.json"

# Relative cost, so the long levels start first and the pool drains evenly. It
# only has to get the ORDER roughly right; anything missing is treated as cheap.
# Measured on the 2026-08-27 full sweep (four workers).
COST = {16: 12.0, 21: 16.0, 22: 11.0, 20: 8.0, 19: 1.6, 18: 1.0, 17: 0.8,
        14: 0.7, 10: 0.6, 11: 0.5, 9: 0.4, 7: 0.5, 8: 0.4, 15: 0.4,
        13: 0.4, 12: 0.3, 1: 0.3, 2: 0.3, 3: 0.3, 4: 0.3, 5: 0.3, 6: 0.3}

# How long a level is allowed to be wrong for. A uniform cap is paid for by the
# FAILING side: with everything at the same budget one broken level burns the
# whole run's wall clock. Per level it is the baseline's own iteration count
# with room for the model moving underneath it.
ITER_MARGIN, ITER_FLOOR, ITER_UNKNOWN = 3, 30, 60

CFG = ["enabled=1", "attempts=1000000", "quitwhendone=1", "blockinput=1",
       "cbs=0", "cos=1", "fastdt=0.0166667", "fastloops=1800", "skiprender=1",
       "music=mute", "servemode=0", "dpsolve=1",
       # This is the research/cold harness, so it asks the mod for the per-attempt band/
       # groups/recorder-input copies explicitly (off by default -- a player's Solve should
       # not accumulate them).
       "capture=1"]

# A from-the-start anchored solve on lv22 legitimately burns 20+ minutes of pure
# DP with no result.txt growth (measured 2026-08-26, five cores busy), and 1200
# reads that as a stall and kills a healthy run.
STALL_S = 2400.0


# The coin-on runs have a baseline of their own. They take different routes and
# different iteration counts, so comparing one against cold_baseline.json says
# nothing (an lv22 coin run printed "baseline 46, +10" against the coin-off
# number). It carries each level's final coin count as well, and a `_meta` entry
# naming what it was blessed from; a run whose coin keys do not match it is
# refused rather than compared with the coin-off file.
BASELINE_COINS = DATA / "cold_baseline_coins.json"
COIN_KEYS = ("coinroute=1", "coins=1")


def coin_cfg(cfg: list[str]) -> bool | None:
    """True for a coin-on run, False for coin off, None when only one of the two
    keys is given (a run that is neither, so there is nothing to compare it to)."""
    have = [k in cfg for k in COIN_KEYS]
    if all(have):
        return True
    return None if any(have) else False


def load_baseline(coins: bool = False) -> dict:
    try:
        return json.loads((BASELINE_COINS if coins else BASELINE).read_text(encoding="utf-8"))
    except (OSError, ValueError):
        return {}


def _all_coins(c: str) -> bool:
    """'3/3' yes, '2/3' or '' no."""
    m = re.fullmatch(r"(\d+)/(\d+)", c or "")
    return bool(m) and m.group(1) == m.group(2) and int(m.group(2)) > 0


def _meta(a, coins: bool, resolution: int | None) -> dict:
    """What a baseline was blessed from, so a later run can tell whether it is
    comparable: the commit, the package that ran, the profile's resolution (GD's
    saw radius follows it), and for a coin baseline the coin keys."""
    run = getattr(a, "run_meta", None) or _run_meta(a)
    m = {"head": run["head"],
         "mod_sha256": run["mod_sha256"],
         "resolution": resolution,
         "levels": list(a.levels), "arrangement": "one-session",
         "record": "none on every level"}
    if coins:
        m["cfg"] = " ".join(COIN_KEYS)
    return m


def _run_meta(a) -> dict:
    """What produced a run: the commit, the package and its sha256, the cfg, the
    levels and the resolution. Written beside a one-session run's log so the
    baseline can be adopted from that run later (--adopt) without running it
    again -- the two-stage bless."""
    import hashlib
    import subprocess
    head = subprocess.run(["git", "rev-parse", "HEAD"], capture_output=True, text=True,
                          cwd=Path(__file__).resolve().parent).stdout.strip()
    # the build's commit when the package carries its provenance (cold_manifest.start)
    prov = getattr(a, "build_prov", None)
    if prov:
        head = prov["commit"]
    meta = {"head": head, "mod": str(a.mod),
            "mod_sha256": hashlib.sha256(Path(a.mod).read_bytes()).hexdigest(),
            "cfg": list(a.cfg), "levels": list(a.levels),
            "resolution": getattr(a, "resolution", None), "arrangement": "one-session"}
    if getattr(a, "fresh_verify", False):
        meta["fresh_verify"] = True       # --adopt then needs each CLEARED level's sidecar
    return meta


def run_resolution(workers: list[int]) -> tuple[int | None, str]:
    """The resolution index the run's workers will play at, and a refusal if
    they disagree. A disposable (minimal) profile without the key gets 25 at
    launch (worker.py), so that is what it will run at."""
    from gdtas import gdsave
    seen = {}
    for w in workers:
        r = gdsave.resolution_index(w)
        if r is None:
            try:
                small = gdsave.save_paths(w)[0].stat().st_size <= 1000
            except OSError:
                small = True
            r = 25 if small else None
        seen[w] = r
    vals = set(seen.values())
    if len(vals) > 1:
        return None, ("the workers play at different resolutions ("
                      + ", ".join(f"{w}: {r}" for w, r in seen.items()) + ")")
    return vals.pop(), ""


def run_geode(workers: list[int]) -> tuple[dict | None, str]:
    """The Geode loader the run's workers will run on (version, Geode.dll hash, which workers), and a refusal if
    they disagree: a run whose arms ran on different loaders measured something the comparison cannot name. The
    pinned loader itself is checked at every launch (gdbase.check_geode); this is the pool as a whole."""
    from gdtas.gdbase import geode_drift_allowed, geode_identity
    from gdtas.paths import worker_dir
    seen = {w: geode_identity(worker_dir(w)) for w in workers}
    shas = {i["dll_sha256"] for i in seen.values()}
    if len(shas) > 1 and not geode_drift_allowed():
        return None, ("the workers run different Geode loaders ("
                      + ", ".join(f"{w}: {i['version']} {str(i['dll_sha256'])[:8]}" for w, i in seen.items()) + ")")
    first = seen[workers[0]]
    return {"version": first["version"], "dll_sha256": first["dll_sha256"], "workers": list(workers)}, ""


def iter_cap(lv: int, base: dict) -> int:
    b = base.get(str(lv), {}).get("iters")
    return max(ITER_FLOOR, ITER_MARGIN * (b if b else ITER_UNKNOWN))


def assign(levels: list[int], pool: list[int]) -> dict[int, list[int]]:
    """Longest-first onto the least loaded worker."""
    load = {w: 0.0 for w in pool}
    out: dict[int, list[int]] = {w: [] for w in pool}
    for lv in sorted(levels, key=lambda l: -COST.get(l, 1.0)):
        w = min(pool, key=lambda x: (load[x], pool.index(x)))
        out[w].append(lv)
        load[w] += COST.get(lv, 1.0)
    return out


def wipe(worker_id: int) -> None:
    """Everything the loop learns lives in the worker's data dir, so a cold run
    starts by deleting it. Leaving one file behind is a seed, and a seeded run
    is not a cold one (CLAUDE.md's first rule).

    Not under a GD that is running: those files are that run's, and the launch would
    refuse the worker ("already running") only after they were gone. Three workers
    share the default pool, so a batch that reaches a busy one is the ordinary case.
    A GD that is only finishing the previous level gets the launch's own 15 seconds."""
    from gdtas.paths import WORKERS_ROOT
    from gdtas.worker import WorkerError, processes_under
    root = WORKERS_ROOT / f"worker-{worker_id}"
    deadline = time.time() + 15.0
    while True:
        pids = processes_under(root)
        if not pids:
            break
        if time.time() >= deadline:
            raise WorkerError(f"worker-{worker_id} is already running (PID {pids}); its data is not wiped. "
                              f"Name a free worker with --pool.")
        time.sleep(1.0)
    d = root / "session" / "data"
    for name in ("dp_seed.txt", "dp_plan.txt", "dp_plan.txt.trace.csv",
                 "dp_tail.txt", "dp_tail.txt.trace.csv", "dp_fixup.trace.csv",
                 "dp_fixups.txt", "dp_fixups_noop.txt", "dp_groups.txt",
                 "dp_groups_deep.txt", "dp_band.txt", "grouptrace.txt",
                 "grouptrace_last.txt", "trace.csv", "dump.csv", "result.txt",
                 "plan_in.txt"):
        try:
            (d / name).unlink(missing_ok=True)
        except OSError:
            pass
    for f in d.glob("solution_lv*_dp.txt"):
        try:
            f.unlink()
        except OSError:
            pass


def _say_guard_stop(r: dict) -> None:
    """A level the runner's guard ended (the memory floor, a level deadline) and that did not clear says so in its `why`: the summary line read "stuck: out of wall time" for a level the
    memory floor stopped at 474 s of a 1200 s budget, which hides that the machine, not the level, ended the run. The reason stays in `stopped`."""
    if r.get("stopped") and not r.get("cleared") and not str(r.get("why", "")).startswith("crash"):
        r["why"] = "stuck: stopped by the guard (" + str(r["stopped"])[:48] + ")"


def one(level: int, wid: int, budget: float, extra: list[str],
        out_dir: Path, mod_file: Path = BUILD_MOD) -> dict:
    t0 = time.time()
    try:
        wipe(wid)
    except WorkerError as e:                    # a worker that is running somebody's session
        return {"lv": level, "cleared": False, "why": f"ERROR {e}",
                "iters": 0, "deepest_t": -1, "deepest_x": -1.0, "fx": 0,
                "record": "?", "wall": time.time() - t0, "fp": "",
                "timeout": False, "died_plan": "", "data": ""}
    guard = run_guard.LevelGuard(GUARD["level_deadline_s"], GUARD["mem_floor_mb"],
                                 out_dir / f"guard_lv{level}.jsonl", first_level=level)
    try:
        r = run_session(wid, CFG + extra + [f"level={level}"],
                        timeout_s=budget, stall_s=STALL_S, mod_file=mod_file,
                        progress=guard.progress, guard=guard)
    except Exception as e:                      # noqa: BLE001  a worker that will not start
        return {"lv": level, "cleared": False, "why": f"ERROR {e}",
                "iters": 0, "deepest_t": -1, "deepest_x": -1.0, "fx": 0,
                "record": "?", "wall": time.time() - t0, "fp": "",
                "timeout": False, "died_plan": "", "data": ""}
    if guard.stopped:
        run_guard.note_stop(out_dir, level, guard.stopped)
    txt = "\n".join(r.lines)
    # The whole session log, next to the results. A stuck level has to be read
    # out of the ladder's own lines, and there is nowhere else they survive.
    try:
        (out_dir / f"coldlog_lv{level}.txt").write_text(txt, encoding="utf-8",
                                                       errors="replace")
    except OSError:
        pass
    out = read_result(txt, getattr(r, "timed_out", False))
    out["lv"] = level
    out["wall"] = time.time() - t0
    crash = getattr(r, "crash", "")
    if crash:
        # Why the launch ended, which the level's own log cannot say (the mod may not have loaded at all): the
        # table says "crash: ..." rather than "stuck", and the whole sentence is kept beside it.
        out["crash"] = crash
        if not out.get("cleared"):
            out["why"] = "crash: " + (crash if len(crash) <= 60 else crash[:57] + "...")
    if guard.stopped:
        out["stopped"] = guard.stopped["why"]
        _say_guard_stop(out)
    run_guard.note_walls(out_dir, {level: out["wall"]})
    # wipe() runs at the START of a run, so the loser's working files are still
    # there when this returns. Naming the directory is the whole handover: the
    # next day's A/B and fixcensus start from it.
    from gdtas.paths import WORKERS_ROOT
    d = WORKERS_ROOT / f"worker-{wid}" / "session" / "data"
    out["data"] = str(d)
    try:
        died = sorted(d.glob("dp_died_it*_t*.txt"),
                      key=lambda f: f.stat().st_mtime)
        out["died_plan"] = died[-1].name if died else ""
    except OSError:
        pass
    # A LEVEL THAT LOST ITS CLOCK HAS TO KEEP ITS EVIDENCE SOMEWHERE THE NEXT
    # RUN CANNOT REACH. Naming the worker's data dir is not keeping it: wipe()
    # empties that dir at the START of the next run on the same worker, and on
    # 2026-09-04 the next run came four minutes later and destroyed the died
    # plan and the dump an investigation was already using. Copy what is needed
    # to rebuild the window -- the plan, the dump the anchor comes from, the
    # moving-geometry recording the model read, and the log.
    if out.get("timeout"):
        keep = out_dir / f"timeout_lv{level}_{time.strftime('%m%d_%H%M%S')}"
        saved, skipped = [], []
        try:
            keep.mkdir(parents=True, exist_ok=True)
            names = [out["died_plan"]] if out.get("died_plan") else []
            names += ["dump.csv", "result.txt", "dp_groups.txt",
                      "dp_plan.txt", "dp_fixups.txt"]
            for n in names:
                src = d / n
                if not (n and src.exists()):
                    continue
                # The dump of a long level runs to hundreds of MB; past this it
                # is cheaper to re-record than to copy, and saying so beats a
                # silent omission.
                if src.stat().st_size > 512 * 1024 * 1024:
                    skipped.append(f"{n} ({src.stat().st_size / 1e6:.0f} MB)")
                    continue
                shutil.copy2(src, keep / n)
                saved.append(n)
            out["kept"] = str(keep)
            out["kept_files"] = saved
            out["kept_skipped"] = skipped
        except OSError as e:                        # noqa: BLE001
            out["kept"] = f"(could not save: {e})"
    return out


# The mod's per-level marker inside a suite log: `suite: level=18 (7/22)`. Its
# own end marker is `suite: done`, which is what run_session waits for -- see the
# note on done_marker there.
SUITE_MARK = re.compile(r"^suite: level=(\d+) \(\d+/\d+\)$", re.M)
SUITE_DONE = "suite: done"


def split_suite(txt: str) -> list[tuple[int, str]]:
    """Cut one process's result.txt into the per-level sessions, in order.

    A level's section runs from its own marker to the next one. The
    `suite: next level=` line the mod writes on the way out belongs to the
    section that wrote it, which is what the anchored `^suite: level=` keeps
    (`suite: next level=` does not match it)."""
    marks = list(SUITE_MARK.finditer(txt))
    out = []
    for i, m in enumerate(marks):
        end = marks[i + 1].start() if i + 1 < len(marks) else len(txt)
        out.append((int(m.group(1)), txt[m.start():end]))
    return out


def one_session(levels: list[int], wid: int, budget: float, extra: list[str],
                out_dir: Path, mod_file: Path = BUILD_MOD) -> list[dict]:
    """Every level in one game. Returns one entry per REQUESTED level, so a suite
    that ends early (a crash, the wall clock) reports the levels it never reached
    as failures rather than as absences.

    The runner's guard (GUARD, py/run_guard.py) can end the game early. A level past
    its deadline is reported as a TIMEOUT and the suite goes on with the levels after
    it in a NEW game -- the run is no longer one game from there, which the manifest's
    `stops` and the result's `stopped` say. Below the memory floor the arm ends: the
    remaining levels are reported as not reached. The suite's own wall clock still
    applies to everything as the last resort."""
    t_end = time.time() + budget
    remaining = list(levels)
    results: list[dict] = []
    n_part = 0
    while remaining:
        part, guard = _one_session_pass(remaining, wid, max(60.0, t_end - time.time()),
                                        extra, out_dir, mod_file, n_part)
        n_part += 1
        stop = guard.stopped
        if stop and guard.level not in remaining:
            # stopped before the first level marker (the memory floor at start-up)
            run_guard.note_stop(out_dir, -1, stop)
            for r in part:
                r["stopped"] = stop["why"]
                _say_guard_stop(r)
        if not stop or guard.level not in remaining:
            results += part
            break
        run_guard.note_stop(out_dir, guard.level, stop)
        cut = remaining.index(guard.level)
        for r in part[:cut + 1]:
            if r["lv"] == guard.level:
                r["stopped"] = stop["why"]
                r["timeout"] = True
                _say_guard_stop(r)
        results += part[:cut + 1]
        rest = remaining[cut + 1:]
        if guard.memory_stop or time.time() >= t_end:
            for lv in rest:
                results.append({"lv": lv, "cleared": False,
                                "why": f"not reached ({stop['why']})",
                                "iters": 0, "deepest_t": -1, "deepest_x": -1.0,
                                "fx": 0, "record": "none", "wall": 0.0, "fp": ""})
            break
        remaining = rest
    run_guard.note_walls(out_dir, {r["lv"]: r.get("wall", 0.0) for r in results
                                   if r.get("wall")})
    return results


def _one_session_pass(levels: list[int], wid: int, budget: float, extra: list[str],
                      out_dir: Path, mod_file: Path = BUILD_MOD, part: int = 0):
    """One game over `levels`: (one entry per requested level, the guard it ran under)."""
    t0 = time.time()
    guard = run_guard.LevelGuard(GUARD["level_deadline_s"], GUARD["mem_floor_mb"],
                                 out_dir / "guard_suite.jsonl", first_level=None)
    try:
        wipe(wid)
    except WorkerError as e:                    # a worker that is running somebody's session
        return [{"lv": lv, "cleared": False, "why": f"ERROR {e}", "iters": 0,
                 "deepest_t": -1, "deepest_x": -1.0, "fx": 0, "record": "?",
                 "wall": time.time() - t0, "fp": ""} for lv in levels], guard
    levels_arg = "levels=" + ",".join(str(l) for l in levels)
    # Per-level wall clock. The log carries no timestamps of its own, so the
    # only clock is this side: run_session reads what result.txt gained about
    # once a second and hands the complete new lines to `progress`, and a level
    # starts at the first poll its own `suite: level=` marker arrives in.
    #
    # Resolution is that poll interval widened by however long the write sits
    # in a buffer -- bounded by the ~10s heartbeat the stall detector already
    # depends on. That is fit for the ONE question this column is asked, "did
    # any level come near the cap", and NOT fit for comparing two runs by.
    seen_at: dict[int, float] = {}

    def _stamp(txt: str) -> None:
        for m in SUITE_MARK.finditer(txt):
            seen_at.setdefault(int(m.group(1)), time.time() - t0)
        guard.progress(txt)

    try:
        r = run_session(wid, CFG + extra + [levels_arg], timeout_s=budget,
                        stall_s=STALL_S, mod_file=mod_file,
                        done_marker=SUITE_DONE, progress=_stamp, guard=guard)
    except Exception as e:                      # noqa: BLE001  a worker that will not start
        return [{"lv": lv, "cleared": False, "why": f"ERROR {e}", "iters": 0,
                 "deepest_t": -1, "deepest_x": -1.0, "fx": 0, "record": "?",
                 "wall": time.time() - t0, "fp": ""} for lv in levels], guard
    if getattr(r, "crash", ""):
        print(f"  worker-{wid}: the game ended: {r.crash}", flush=True)
    txt = "\n".join(r.lines)
    # A suite the guard split runs more than one game; each keeps its own log.
    suite_log = out_dir / ("coldlog_suite.txt" if part == 0
                           else f"coldlog_suite_part{part}.txt")
    try:
        suite_log.write_text(txt, encoding="utf-8", errors="replace")
    except OSError:
        pass
    timed_out = getattr(r, "timed_out", False)
    sections = split_suite(txt)
    seen = {lv: sec for lv, sec in sections}
    # Only the LAST section can have been cut off by the wall clock; the earlier
    # ones ended by themselves or the suite would never have moved on.
    last_lv = sections[-1][0] if sections else None
    total = time.time() - t0
    wall_each = total / max(1, len(sections))
    order = [lv for lv, _ in sections]

    def _wall(lv: int) -> float:
        """Seconds this level held the game: its own marker to the next one's
        (the last level's end is the run's). Falls back to the even share when
        the stamp is missing, so a level is never reported as 0s just because
        the callback never fired."""
        if lv not in seen_at:
            return wall_each
        i = order.index(lv) if lv in order else -1
        nxt = order[i + 1] if 0 <= i < len(order) - 1 else None
        end = total if nxt is None else seen_at.get(nxt, total)
        return max(0.0, end - seen_at[lv])

    results = []
    for lv in levels:
        sec = seen.get(lv)
        if sec is None:
            results.append({"lv": lv, "cleared": False,
                            "why": "not reached (the suite ended early)",
                            "iters": 0, "deepest_t": -1, "deepest_x": -1.0,
                            "fx": 0, "record": "none", "wall": 0.0, "fp": ""})
            continue
        try:
            (out_dir / f"coldlog_lv{lv}.txt").write_text(sec, encoding="utf-8",
                                                         errors="replace")
        except OSError:
            pass
        out = read_result(sec, timed_out and lv == last_lv)
        out["lv"] = lv
        # Measured from the marker stamps above, not the even share. The share
        # made every level look average, which is exactly the shape that hides
        # a single level sitting near the cap.
        out["wall"] = _wall(lv)
        results.append(out)
    return results, guard


def read_result(txt: str, timed_out: bool = False) -> dict:
    """Read one session's log into the numbers a cold run is judged by.

    Split out of one() so that the private tree's custom-level runner reads a
    session EXACTLY the same way (docs/NOTES.md; custom levels are not supported
    here yet). The point of that runner is that the same loop is being measured
    and only the level source differs, which is worth nothing if the two
    disagree about what counts as cleared."""
    iters = re.findall(r"^dpsolve: iter (\d+):", txt, re.M)
    deaths = [(int(a), float(b)) for a, b in
              re.findall(r"^death: attempt=\d+ tick=(\d+) x=([\d.]+)", txt, re.M)]
    fps = re.findall(r"^dpsolve:   \[fp\] (.*)$", txt, re.M)
    # The coins the game counted when the level was finished. The LAST line: an
    # earlier one can be the bootstrap recording's pass reaching the end without
    # trying for coins (0/3 on lv19-21), which is not the solve.
    coins = re.findall(r"^coingd: level complete with (\d+/\d+) coins", txt, re.M)
    saved = bool(re.search(r"^dpsolve: solution saved", txt, re.M))
    why = "CLEARED" if saved else "stuck"
    if not saved:
        m = re.search(r"^dpsolve: giving up - (.*)$", txt, re.M)
        if m:
            why = "stuck: " + m.group(1)[:60]
        elif timed_out:
            why = "stuck: out of wall time"
    rec = re.search(r"^level record changed: (.*)$", txt, re.M)
    # The audit line is written when the SESSION closes. A run the harness killed
    # at its wall-clock budget never gets there, and reporting that absence as a
    # leak is a false alarm on the one gate that must stay believable: on
    # 2026-09-03 a budget-killed lv22 smoke printed `RECORD GATE LEAKED (?)` while
    # the worker's save file had not been written since 2026-08-29. So the two
    # cases are named differently -- a session that ENDED and still wrote no audit
    # line is a real anomaly and keeps failing the run; one that was killed has no
    # verdict at all, in either direction.
    ended = bool(re.search(r"^(session_end|suite: done)", txt, re.M))
    if rec:
        record = rec.group(1)
    elif ended:
        record = "MISSING (session ended without the audit line)"
    else:
        record = "no-audit (killed before session end)"
    # The Area Move envelope's own check (src/solver/areaenv.hpp), on levels that have one. Any
    # count below other than zero means the box was not verified against the game, and a level
    # solved against an unverified box is not seed-independent. `solver_box_kills` is not a
    # fault: the box is conservative by design, and a non-zero count says the search was pruned
    # by uncertainty and not only by the level.
    areaenv = ""
    ae = re.search(r"^areaenv: (.*)$", txt, re.M)
    if ae:
        f = dict(re.findall(r"(\w+)=(\d+)", ae.group(1)))
        dirty = [k for k in ("miss", "outside_box", "value_miss", "offset_miss", "rect_miss",
                             "compound", "unenveloped") if int(f.get(k, 0))]
        kills = int(f.get("solver_box_kills", 0))
        areaenv = ("NOT CLEAN: " + ", ".join(f"{k}={f[k]}" for k in dirty)) if dirty else \
            ("clean" + (f", {kills} solver kills by the box (pruned by uncertainty)"
                        if kills else ", no solver kills by the box"))
    return {"cleared": saved, "why": why, "iters": len(iters),
            "areaenv": areaenv,
            # A level stopped by the clock is not the same finding as one the
            # search gave up on: the first says "too slow to be worth waiting
            # for today", the second says "the model cannot get through". They
            # were both `stuck` and read as one number.
            "timeout": bool(timed_out) and not saved,
            # Filled in by one() from the data dir: the loop WRITES the died
            # plans but never names them in the log, so reading them out of the
            # text would have been a field that is empty forever.
            "died_plan": "",
            "deepest_t": max((d[0] for d in deaths), default=-1),
            "deepest_x": max((d[1] for d in deaths), default=-1.0),
            "fx": len(re.findall(r"\[fixup\] t=\d+ x=", txt)),
            "record": record,
            "coins": coins[-1] if coins else "",
            # The last [fp] is the state the run ended in -- the cheapest single
            # value to diff two runs by (see logFingerprint in repair.hpp).
            "fp": fps[-1] if fps else ""}


FRESH_TIMEOUT_S = 900.0
FRESH_SIDECAR = "fresh_verify_lv{}.json"
FRESH_LOG = "fresh_replay_lv{}.txt"


def _sha256(p: Path) -> str:
    import hashlib
    return hashlib.sha256(Path(p).read_bytes()).hexdigest()


def fresh_classify(lines: list[str], clear: bool, detail: str, timed_out: bool,
                   stall: dict | None = None) -> tuple[str, str, dict | None]:
    """What a replay's log says, as (kind, detail, death). The two kinds that are about the PLAN:
    `death` (the replay died: tick, x, and who when the game said) and `no_complete` (the game ended
    with neither a death nor a completion). `instrument` is the clock ending it: the replay did not
    finish, and nothing is claimed about the plan."""
    if clear:
        return "ok", detail or "", None
    if timed_out and stall:
        # the launch was ended because its log went quiet, not because the replay's time was spent: say where it stopped and under what load
        return ("instrument", f"stalled: result.txt quiet for {stall.get('quiet_s')}s after \"{stall.get('last_line', '')[:80]}\" "
                f"({stall.get('gd_running')} game process(es) on the machine, {stall.get('mem_free_gb')} GB free)", None)
    if timed_out:
        return "instrument", f"timeout ({detail})", None
    death = None
    who = ""
    for ln in lines:
        m = re.match(r"killer: t=(\d+) who=(\w+)", ln)
        if m:
            who = m.group(2)
        m = re.match(r"death: attempt=\d+ tick=(\d+) x=([\d.]+)", ln)
        if m and death is None:
            death = {"tick": int(m.group(1)), "x": float(m.group(2)), "who": who}
    if death:
        return "death", f"died at t={death['tick']} x={death['x']:.1f}" + (f" ({death['who']})" if death["who"] else ""), death
    return "no_complete", f"the replay ended with no completion and no death ({detail})", None


def fresh_apply(r: dict, side: dict) -> None:
    """Put a fresh-verify sidecar's verdict on a result. A replay that died or never completed is
    `stuck (mid_session_only)`; an instrument failure (no plan, the game would not start, the
    clock) is `unverified`, which says nothing about the plan and is still not a clear."""
    r["fresh_verify"] = side["kind"] if side["kind"] != "ok" else "ok"
    r["fresh_verify_detail"] = side.get("detail", "")
    if side["kind"] == "ok":
        return
    r["cleared"] = False
    r["why"] = ("stuck (mid_session_only)" if side["kind"] in ("death", "no_complete")
                else f"unverified (fresh-verify: {side.get('detail', '')[:60]})")


def fresh_verify(results: list[dict], wid: int, mod_file: Path, out_dir: Path,
                 extra_cfg: list[str] | None = None, data_dir: Path | None = None) -> None:
    """--fresh-verify: after the solve, play each saved solution once from the start in a NEW game.

    `dpsolve: solution saved` is the loop's own claim, made in the session that solved the
    level, and a level can be cleared there and not from the start: the loop re-anchors on the
    game's state (a restored checkpoint), so a state the restore built can differ from the one
    the plan reaches by playing from tick 0 (a custom two-player level: a P2 that dies at
    t=27928 when the plan is played from the head). The replay is the bare one -- solve=0,
    blockinput, the cfg input path -- and does not inherit the solver's cfg, which is what
    verify_solutions.verify_one builds. A level whose replay dies or never completes is reported
    `stuck (mid_session_only)`: it cleared in the session and nowhere else. A replay that could
    not be made (no plan file, the game would not start, the clock) is `unverified` -- a failed
    instrument, not a finding about the plan.

    The evidence is kept beside the logs, per level, so the verdict can be read again without
    running the game (and --adopt does): fresh_verify_lv<N>.json (the request, the package's and the
    plan's SHA256, the kind and the reason) and fresh_replay_lv<N>.txt (the replay's whole log).

    A development tool's check: nothing here is reached from the Play menu, and the mod's own
    clear gate is not touched. The worker must be free (the suite's game has ended); the solution
    is read from its data dir, where the next level's wipe() would delete it, so a per-level run
    calls this right after the level."""
    from gdtas.paths import WORKERS_ROOT
    from verify_solutions import verify_one
    data = data_dir or (WORKERS_ROOT / f"worker-{wid}" / "session" / "data")
    mod_sha = _sha256(mod_file) if Path(mod_file).exists() else ""
    for r in results:
        if not r.get("cleared"):
            continue
        lv = r["lv"]
        src = data / f"solution_lv{lv}_dp.txt"
        side = {"requested": True, "level": lv, "mod": str(mod_file), "mod_sha256": mod_sha,
                "plan": src.name, "plan_sha256": "", "kind": "", "detail": "", "death": None,
                "log": ""}
        if not src.exists():
            side["kind"], side["detail"] = "instrument", f"no {src.name} in {data}"
        else:
            side["plan_sha256"] = _sha256(src)
            try:
                shutil.copy2(src, out_dir / src.name)       # the solution that was judged, beside the logs
            except OSError:
                pass
            try:
                v = verify_one(wid, src, lv, FRESH_TIMEOUT_S, WORKERS_ROOT,
                               extra_cfg=list(extra_cfg or []), mod_file=mod_file)
                (out_dir / FRESH_LOG.format(lv)).write_text("\n".join(v.lines), encoding="utf-8", errors="replace")
                side["log"] = FRESH_LOG.format(lv)
                side["kind"], side["detail"], side["death"] = fresh_classify(v.lines, v.clear, v.detail, v.timed_out,
                                                                           getattr(v, "stall", None))
                if getattr(v, "stall", None):
                    side["stall"] = v.stall
            except Exception as e:                          # noqa: BLE001  a worker that will not start
                side["kind"], side["detail"] = "instrument", f"ERROR {e}"
        (out_dir / FRESH_SIDECAR.format(lv)).write_text(json.dumps(side, indent=1), encoding="utf-8")
        print(f"  fresh-verify lv{lv}: {'ok' if side['kind'] == 'ok' else side['kind'].upper() + ' ' + side['detail'][:120]}",
              flush=True)
        fresh_apply(r, side)

# The bare replay's cfg (verify_solutions.verify_one builds the same list, plus the level and its plan); test_cold_regress_verdict holds the two together.
FRESH_BARE_CFG = ["enabled=1", "attempts=1", "quitwhendone=1", "blockinput=1", "cbs=0", "cos=1", "notrace=1",
                  "fastdt=0.0166667", "fastloops=1800", "skiprender=1", "solve=0", "music=mute", "coins=0"]


def fresh_verify_one_session(results: list[dict], wid: int, mod_file: Path, out_dir: Path,
                             extra_cfg: list[str] | None = None, data_dir: Path | None = None,
                             retry: bool = True) -> dict:
    """--fresh-verify-one-session: the saved solutions of every cleared level, played from the start, all in ONE new game.

    The same bare replay as fresh_verify (solve=0, blockinput, a new process that did not solve), but the game is started once: the mod's suite
    (`levels=`) enters each level in turn and reads that level's plan from the directory named by `suiteplans=` (mod/config.hpp suite::g_planDir).
    A launch that stalls at its start has been seen (a menu that never enters the level, a level that never loads), and a suite has one start where
    the per-level form has twenty-two. What is lost is the independent launch per level; a state one level leaves behind is looked for in the
    one-session cold run.

    The verdict of each level is read from its own section of the log (split_suite) with the classifier fresh_classify uses, and each level gets the
    sidecar and the replay log fresh_verify writes (plus `arrangement: one-session`). A game that stalls or ends early leaves the level it was in and
    the levels after it `instrument`; with `retry` those are played once more by fresh_verify, one launch each, and the sidecar keeps what the
    one-session attempt said (`one_session_first`) and that the retry was used. The count of stalls and of retried levels is printed and written to
    fresh_verify_suite.json, so a retry never hides how often the game stalls."""
    from gdtas import results as gres
    from gdtas.paths import WORKERS_ROOT
    data = data_dir or (WORKERS_ROOT / f"worker-{wid}" / "session" / "data")
    mod_sha = _sha256(mod_file) if Path(mod_file).exists() else ""
    plan_dir = out_dir / "fresh_plans"
    plan_dir.mkdir(parents=True, exist_ok=True)
    sides: dict[int, dict] = {}
    ready: list[int] = []
    for r in results:
        if not r.get("cleared"):
            continue
        lv = r["lv"]
        src = data / f"solution_lv{lv}_dp.txt"
        side = {"requested": True, "level": lv, "mod": str(mod_file), "mod_sha256": mod_sha, "plan": src.name, "plan_sha256": "",
                "kind": "", "detail": "", "death": None, "log": "", "arrangement": "one-session"}
        if not src.exists():
            side["kind"], side["detail"] = "instrument", f"no {src.name} in {data}"
        else:
            side["plan_sha256"] = _sha256(src)
            for dst in (out_dir / src.name, plan_dir / src.name):
                try:
                    shutil.copy2(src, dst)
                except OSError:
                    pass
            ready.append(lv)
        sides[lv] = side
    stall = None
    ended = ""
    gave_up = False
    if ready:
        cfg = (FRESH_BARE_CFG + list(extra_cfg or []) + ["levels=" + ",".join(str(lv) for lv in ready),
                                                         "suiteplans=" + plan_dir.resolve().as_posix()])
        try:
            rs = run_session(wid, cfg, timeout_s=FRESH_TIMEOUT_S + 120.0 * len(ready), workers_root=WORKERS_ROOT,
                             mod_file=mod_file, done_marker=SUITE_DONE)
            txt = "\n".join(rs.lines)
            stall = getattr(rs, "stall", None)
            timed_out = bool(getattr(rs, "timed_out", False))
            ended = getattr(rs, "crash", "") or ""
            sections = dict(split_suite(txt))
            last_lv = max(sections, key=lambda k: txt.find(f"suite: level={k} ")) if sections else None
            # The mod's suite cannot leave a level whose replay ended in a death (the dead PlayLayer stays up): it says so and stops. That is the game's
            # answer, not a stall of the launch, and the levels after the dead one are the ones played again by themselves.
            gave_up = "suite: gave up waiting to leave the level" in txt
            for lv in ready:
                side = sides[lv]
                sec = sections.get(lv)
                if sec is None:
                    why = (f"stalled before this level (result.txt quiet {stall.get('quiet_s')}s after \"{stall.get('last_line', '')[:60]}\")" if stall
                           else (f"the game ended: {ended}" if ended else
                                 (f"the suite stopped after lv{last_lv}: it could not leave the level the game was in (a replay that ended in a death)" if gave_up
                                  else ("the clock ended the suite" if timed_out else "the suite ended"))))
                    side["kind"], side["detail"] = "instrument", f"not reached: {why}"
                    continue
                lines = sec.split("\n")
                clear, detail = gres.clear_verdict(lines)
                (out_dir / FRESH_LOG.format(lv)).write_text(sec, encoding="utf-8", errors="replace")
                side["log"] = FRESH_LOG.format(lv)
                here_out = timed_out and lv == last_lv
                side["kind"], side["detail"], side["death"] = fresh_classify(lines, clear, detail, here_out,
                                                                           stall if here_out else None)
                if here_out and stall:
                    side["stall"] = stall
        except Exception as e:                              # noqa: BLE001  a worker that will not start
            for lv in ready:
                sides[lv]["kind"], sides[lv]["detail"] = "instrument", f"ERROR {e}"
    redo = [lv for lv in ready if sides[lv]["kind"] == "instrument"] if retry else []
    for r in results:
        lv = r.get("lv")
        if lv not in sides:
            continue
        side = sides[lv]
        if lv in redo:
            continue
        (out_dir / FRESH_SIDECAR.format(lv)).write_text(json.dumps(side, indent=1), encoding="utf-8")
        print(f"  fresh-verify lv{lv}: {'ok' if side['kind'] == 'ok' else side['kind'].upper() + ' ' + side['detail'][:120]}", flush=True)
        fresh_apply(r, side)
    if redo:
        for lv in redo:
            first = {k: sides[lv].get(k) for k in ("kind", "detail", "stall")}
            print(f"  fresh-verify lv{lv}: one-session attempt was {first['kind'].upper()} ({str(first['detail'])[:100]}); one more launch by itself", flush=True)
        fresh_verify([r for r in results if r.get("lv") in redo], wid, mod_file, out_dir, extra_cfg, data_dir)
        for lv in redo:
            p = out_dir / FRESH_SIDECAR.format(lv)
            try:
                d = json.loads(p.read_text(encoding="utf-8"))
                d["one_session_first"] = {k: sides[lv].get(k) for k in ("kind", "detail", "stall")}
                d["per_launch_retry"] = True
                d["arrangement"] = "one-session, then per-launch"
                p.write_text(json.dumps(d, indent=1), encoding="utf-8")
            except (OSError, ValueError):
                pass
    summary = {"arrangement": "one-session", "levels": ready, "stalled": bool(stall), "stall": stall, "game_ended": ended,
               "stopped_after_a_level_it_could_not_leave": bool(ready) and gave_up,
               "retried_per_launch": redo, "kinds_first_attempt": {str(lv): sides[lv]["kind"] for lv in ready}}
    (out_dir / "fresh_verify_suite.json").write_text(json.dumps(summary, indent=1), encoding="utf-8")
    print(f"  fresh-verify one-session: {len(ready)} level(s) in one game, {1 if stall else 0} stall(s)"
          + (f", per-launch retry for {redo}" if redo else ", no retry"), flush=True)
    return summary


def report(results: list[dict], base: dict, a) -> int:
    """The verdict, shared by both arrangements so they are judged identically."""
    results.sort(key=lambda r: r["lv"])
    coins = bool(coin_cfg(list(a.cfg)))
    print("\n=== cold regression ===" + (f" (coins, against {BASELINE_COINS.name})"
                                          if coins else ""))
    bad: list[str] = []
    over_cap: list[str] = []
    timeouts: list[int] = []
    for r in results:
        b = base.get(str(r["lv"]), {})
        mark = ""
        # `is not None`, not truthiness: A BASELINE OF 0 IS A BASELINE. Eight of
        # the 22 levels are free in the baseline (lv2/4/5/6/8/9/13/17), and under
        # the old test every one of them was skipped entirely -- no delta printed
        # and NO CAP CHECK -- so a level could go from 0 iterations to any number
        # and the run still printed PASS without a word. Measured on 2026-09-14:
        # lv17 went 0 -> 21 (0 -> 24 fixups) under the wave kill-box changes and
        # the report said nothing, while lv18's +1 was annotated. iter_cap
        # already handles the zero (it reads ITER_UNKNOWN there, a cap of 180).
        if b.get("iters") is not None and r["cleared"]:
            d = r["iters"] - b["iters"]
            if d:
                mark = f"  (baseline {b['iters']}, {d:+d})"
            # Over the cap is a thing to look at, not a failure: iterations are
            # not a verdict.
            # The wall and stall clocks are what protect the machine.
            if r["iters"] > iter_cap(r["lv"], base):
                over_cap.append(f"lv{r['lv']}: {r['iters']} iterations against a cap of "
                                f"{iter_cap(r['lv'], base)}")
        # A coin run is judged on its coins too: the baseline holds each level's
        # final count, and fewer is a failure whatever the iterations say.
        if coins and r["cleared"] and b.get("coins") and r.get("coins") != b["coins"]:
            bad.append(f"lv{r['lv']}: coins {r.get('coins') or 'none'} against the "
                       f"baseline's {b['coins']}")
        kind = "CLEARED" if r["cleared"] else \
            ("TIMEOUT" if r.get("timeout") else r["why"])
        print(f"lv{r['lv']:<3} {kind:<40}iters={r['iters']:<4}"
              f"{r.get('wall', 0.0):>6.0f}s{mark}"
              + (f"  coins {r.get('coins') or 'none'}" if coins else ""))
        if r.get("areaenv"):
            print(f"    areaenv: {r['areaenv']}")
            if r["areaenv"].startswith("NOT CLEAN"):
                bad.append(f"lv{r['lv']}: the Area Move box was not verified ({r['areaenv']})")
        if r.get("timeout"):
            # A level over its clock is a signal, not a mystery, and everything
            # the morning needs is already on disk. Print where, and the three
            # values that pick the next experiment (user's ruling 2026-09-03:
            # drop it early and go and fix it, rather than waiting it out).
            timeouts.append(r["lv"])
            print((f"    stopped by the guard ({r['stopped']}) after " if r.get("stopped")
                   else f"    over its {r.get('wallcap', 0):.0f}s cap after ")
                  + f"{r['wall']:.0f}s: deepest x={r['deepest_x']:.0f} "
                  f"t={r['deepest_t']}, {r['fx']} fixups"
                  + (f", last died plan {r['died_plan']}"
                     if r["died_plan"] else ""))
            if r["fp"]:
                print(f"    last [fp] {r['fp'][:110]}")
            if r.get("kept"):
                print(f"    copied to {r['kept']}: "
                      f"{', '.join(r.get('kept_files') or []) or 'nothing'}"
                      + (f" (too big to copy: "
                         f"{', '.join(r['kept_skipped'])})"
                         if r.get("kept_skipped") else ""))
            if r.get("data"):
                print(f"    (the worker's own dir {r['data']} is emptied by "
                      f"the next run on that worker)")
        if not r["cleared"]:
            bad.append(f"lv{r['lv']}: "
                       + (f"TIMEOUT after {r['wall']:.0f}s" if r.get("timeout")
                          else r["why"])
                       + (f" [{r['stopped']}]" if r.get("stopped") else ""))
        if r["record"].startswith("no-audit"):
            # No verdict either way -- say so, and do not add a second failure to
            # a run that is already failing for the reason it was killed.
            print(f"lv{r['lv']:<3} record gate: {r['record']} -- this run says "
                  f"nothing about the gate")
        elif r["record"] != "none" and not r["record"].startswith("none"):
            bad.append(f"lv{r['lv']}: RECORD GATE LEAKED ({r['record']})")

    # Which level was expensive, and did anything come near its clock. The
    # ruling "re-run a level that came near the cap on its own" needs a name to
    # act on, and reading it off 22 printed numbers by eye is how it stopped
    # being executed at all.
    #
    # NOTE the two arrangements cap DIFFERENT things. Per level (--pool) there
    # is a real per-level cap and `wallcap` carries it. In --one-session the
    # budget is multiplied out into a cap on the WHOLE suite, so no per-level
    # cap exists there and "near the cap" is not a question that arrangement can
    # answer -- the most expensive level is still worth naming.
    timed = [r for r in results if r.get("wall")]
    if timed:
        top = max(timed, key=lambda r: r["wall"])
        print(f"\nwall clock: {sum(r['wall'] for r in timed) / 60.0:.0f} min "
              f"total, dearest lv{top['lv']} at {top['wall']:.0f}s")
        near = [r for r in timed
                if r.get("wallcap") and r["wall"] >= 0.8 * r["wallcap"]]
        if near:
            print("  near the per-level cap -- re-run each on its own: "
                  + ", ".join(f"lv{r['lv']} ({r['wall']:.0f}s of "
                              f"{r['wallcap']:.0f}s)" for r in near))
        elif not any(r.get("wallcap") for r in timed):
            print("  (one session: the cap is on the suite, not per level)")

    # Count the result lines. A worker that hung or crashed reports nothing at
    # all, so a short run must never read as a clean one. (A suite fills in the
    # levels it never reached, so there they are already counted as failures.)
    if len(results) != len(a.levels):
        missing = sorted(set(a.levels) - {r["lv"] for r in results})
        print(f"INCOMPLETE: {len(results)} of {len(a.levels)} levels reported; "
              f"missing {missing}. Check free memory first, then shrink --pool.")
        return 1

    if a.bless:
        # Only bless a complete, clean run. A baseline built out of a run that
        # failed somewhere records the failure as the expectation, and the next
        # run then compares itself against it and passes.
        # A coin baseline also needs every level to have ended with all its
        # coins: blessing a 2/3 would make the miss the expectation.
        short = [f"lv{r['lv']} {r.get('coins') or 'none'}" for r in results
                 if coins and r["cleared"] and not _all_coins(r.get("coins", ""))]
        # A baseline has to say where it was measured.
        if getattr(a, "resolution", None) is None:
            bad.append("the profile's resolution is unknown, so a baseline from this "
                       "run could not say where it was measured")
        if bad or short:
            print("\nNOT BLESSED: this run is not clean (see FAIL below)"
                  + (f"; not every level ended with all its coins: {', '.join(short)}"
                     if short else ""))
        else:
            out = {str(r["lv"]): {"iters": r["iters"], "fp": r["fp"]}
                   for r in results if r["cleared"]}
            path = BASELINE
            if coins:
                for r in results:
                    if r["cleared"]:
                        out[str(r["lv"])]["coins"] = r["coins"]
                path = BASELINE_COINS
            out["_meta"] = _meta(a, coins, getattr(a, "resolution", None))
            path.write_text(json.dumps(out, indent=1, sort_keys=True),
                            encoding="utf-8")
            print(f"blessed {len([k for k in out if k != '_meta'])} levels -> {path}")

    if timeouts:
        print(f"\nover the per-level cap: {len(timeouts)} "
              f"({', '.join('lv' + str(l) for l in timeouts)}) -- each one's "
              f"working files are named above")
    if over_cap:
        # Printed, never counted as a failure: a level far over its baseline is
        # worth a look (and an iteration count that grows on every run is the
        # signal), but whether a change was good is not decided by it.
        print("\nover the iteration cap (not a failure -- worth a look): "
              + "; ".join(over_cap))
    if bad:
        print("\nFAIL:")
        for b in bad:
            print("  " + b)
        return 1
    print(f"\nPASS ({len(results)}/{len(results)} cleared cold)")
    return 0


def adopt(a) -> int:
    """Bless from a saved --one-session run (the folder --adopt names), judged by
    the same report() a live run is: complete, cleared, record none, all coins on
    a coin run, a known resolution. The run's own commit, package and resolution
    go into the baseline's _meta, not today's."""
    import hashlib
    d = Path(a.adopt)
    log, meta_p = d / "coldlog_suite.txt", d / "coldlog_suite.meta.json"
    if not log.exists() or not meta_p.exists():
        print(f"refused: {d} needs coldlog_suite.txt and coldlog_suite.meta.json "
              f"(both written to data/ by a --one-session run)")
        return 2
    run = json.loads(meta_p.read_text(encoding="utf-8"))
    mod = Path(run["mod"])
    if mod.exists() and hashlib.sha256(mod.read_bytes()).hexdigest() != run["mod_sha256"]:
        print(f"refused: {mod} is no longer the package that run measured")
        return 2
    a.cfg, a.levels, a.mod = list(run["cfg"]), list(run["levels"]), mod
    a.resolution, a.run_meta, a.bless = run.get("resolution"), run, True
    coins = coin_cfg(a.cfg)
    if coins is None:
        print("refused: the run's cfg names only one of the coin keys")
        return 2
    if a.resolution is None:
        print("refused: the run does not say which resolution it was measured at")
        return 2
    print(f"adopting from {d}: run of {run['head'][:7]}, {mod.name} "
          f"{run['mod_sha256'][:8]}, resolution {a.resolution}, cfg {' '.join(a.cfg) or '-'}")
    results = []
    for lv, sec in split_suite(log.read_text(encoding="utf-8", errors="replace")):
        r = read_result(sec)
        r["lv"], r["wall"] = lv, 0.0
        results.append(r)
    if run.get("fresh_verify"):
        # The run asked for the replay from the start, so a CLEARED the log alone shows is not
        # the verdict: it is whatever the replay said, kept in the sidecar. Without the sidecar
        # (or with one for another package) the verdict is unknown, and unknown is refused.
        for r in results:
            if not r["cleared"]:
                continue
            sp = d / FRESH_SIDECAR.format(r["lv"])
            if not sp.exists():
                print(f"refused: the run asked for --fresh-verify and lv{r['lv']} has no {sp.name} "
                      f"beside the log: whether its solution clears from the start is unknown")
                return 2
            side = json.loads(sp.read_text(encoding="utf-8"))
            if side.get("level") != r["lv"] or side.get("mod_sha256") != run["mod_sha256"] \
                    or side.get("kind") not in ("ok", "death", "no_complete", "instrument"):
                print(f"refused: {sp.name} is not a fresh-verify verdict of this run's package "
                      f"for lv{r['lv']}")
                return 2
            fresh_apply(r, side)
    return report(results, load_baseline(coins), a)


def add_guard_args(ap) -> None:
    """The runner's own limits inside a launch (py/run_guard.py). Shared with the
    private tree's runners, which call one() / one_session() as they are."""
    ap.add_argument("--level-deadline", type=float, default=None,
                    help="seconds one level may hold the game before it is stopped as a "
                         "TIMEOUT (default: the previous run's longest level times "
                         "--level-deadline-scale, at least --level-deadline-floor, at most "
                         "the per-level budget; 0 = off)")
    ap.add_argument("--level-deadline-scale", type=float, default=3.0)
    ap.add_argument("--level-deadline-floor", type=float, default=600.0)
    ap.add_argument("--mem-floor-gb", type=float, default=3.0,
                    help="end the arm when available physical memory falls below this "
                         "(0 = off)")


def setup_guard(a, prev_manifest: Path, cap_s: float) -> dict:
    """Set GUARD from the parsed arguments and return what was chosen, for the manifest."""
    if a.level_deadline == 0:
        deadline, source = 0.0, "off (command line)"
    else:
        deadline, source = run_guard.pick_deadline(prev_manifest, a.level_deadline_scale,
                                                   a.level_deadline_floor, cap_s,
                                                   a.level_deadline)
    GUARD["level_deadline_s"] = deadline
    GUARD["mem_floor_mb"] = max(0.0, a.mem_floor_gb) * 1024.0
    print(f"  guard: level deadline "
          + (f"{deadline:.0f}s ({source})" if deadline else "off")
          + (f", memory floor {a.mem_floor_gb:g} GB" if a.mem_floor_gb > 0 else
             ", memory floor off"))
    return {"level_deadline_s": round(deadline, 1), "deadline_source": source,
            "mem_floor_mb": GUARD["mem_floor_mb"], "cap_s": cap_s}


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--levels", nargs="+", type=int,
                    default=list(range(1, 23)))
    ap.add_argument("--pool", nargs="+", type=int, default=list(DEFAULT_POOL))
    # THE PER-LEVEL CLOCK. A level that solves but takes an hour is a red
    # signal, not a result worth waiting for: the user's ruling of 2026-09-03
    # is to drop it early, keep what it had, and go and find out why. The cap
    # is about twice the release build's own time -- 20 minutes on a Windows
    # worker; slower workers scale it with --budget-scale (1.5 for ones
    # roughly half as fast again).
    # Left at None the two arrangements resolve it differently, because they
    # mean different things by "budget": in parallel it IS the per-level clock,
    # while --one-session multiplies it by the level count into one cap for the
    # whole game. Passing --budget explicitly overrides both, so nothing that
    # named a number changes.
    # Inside a launch the runner now keeps its own per-level deadline and memory
    # floor as well (--level-deadline / --mem-floor-gb, py/run_guard.py); the
    # suite's cap stays as the last resort.
    ap.add_argument("--budget", type=float, default=None,
                    help="wall-clock seconds per level (default 1200 times "
                         "--budget-scale; in --one-session, seconds per level "
                         "summed into one cap for the whole game, default 3600)")
    ap.add_argument("--budget-scale", type=float, default=1.0,
                    help="multiply the default per-level cap, for workers slower "
                         "than a Windows one (1.5 for half as fast again)")
    ap.add_argument("--cfg", nargs="*", default=[],
                    help="extra autorun.cfg keys, e.g. dpfingerprint=0")
    ap.add_argument("--one-session", action="store_true",
                    help="solve every level in ONE game, in order (implied by --bless)")
    ap.add_argument("--bless", action="store_true",
                    help="save this run's iteration counts as the baseline "
                         "(runs --one-session: see the note at the top)")
    ap.add_argument("--fresh-verify", action="store_true",
                    help="after a level is CLEARED, play its saved solution once from the start "
                         "in a new game; one that does not clear is reported `stuck "
                         "(mid_session_only)` (see fresh_verify). Use it for an acceptance run.")
    ap.add_argument("--fresh-verify-one-session", action="store_true",
                    help="--fresh-verify, but the saved solutions are replayed one after another in ONE new game "
                         "(cfg `suiteplans=`), one launch instead of one per level; a game that stalls leaves the "
                         "levels it did not finish `unverified`, which are played once more by themselves, and the "
                         "stall and the retry are counted (see fresh_verify_one_session). With --one-session only; "
                         "other arrangements use --fresh-verify's own form.")
    # Which binary is being measured is the question this file exists to answer,
    # and it had no way to say. A release built by CI is not the build on the
    # desk -- different configuration, so not obviously the same arithmetic.
    ap.add_argument("--mod", type=Path, default=BUILD_MOD,
                    help="the .geode to run (default: the local build)")
    # The second stage of a bless: a --one-session run leaves
    # coldlog_suite.txt and coldlog_suite.meta.json in data/; copied to a folder
    # and reviewed, the baseline is adopted from them here, without running GD.
    ap.add_argument("--adopt", type=Path, default=None,
                    help="adopt the baseline from a saved --one-session run "
                         "(a folder holding coldlog_suite.txt and coldlog_suite.meta.json)")
    add_guard_args(ap)
    a = ap.parse_args(argv)
    if getattr(a, "fresh_verify_one_session", False):
        a.fresh_verify = True
    if a.adopt:
        return adopt(a)
    if not a.mod.exists():
        print(f"no such package: {a.mod}")
        return 1
    if a.mod != BUILD_MOD:
        print(f"measuring {a.mod}\n  (not the local build at {BUILD_MOD})")
    # PIN THE PACKAGE ONCE, here, and measure the pinned copy from now on. The
    # workers snapshot it by content hash anyway, but they do it at each launch:
    # between this run's meta and that snapshot the main line can rebuild, and
    # then the meta names one package while the game loads another. Reading the
    # immutable copy for both closes that window (the meta being written after
    # the suite -- the other half of this -- was fixed in e2a1dde).
    # ...and read the build's provenance BEFORE pinning: it sits beside the package it was
    # written for (build/provenance.json, copied with an arm's package), not beside the pin.
    a.build_prov = cold_manifest.read_provenance(a.mod)
    if a.build_prov:
        print(f"  built from {a.build_prov['commit'][:12]}"
              f"{' (dirty)' if a.build_prov.get('dirty') else ''} per its provenance.json")
    pinned, digest = snapshot_mod(a.mod)
    if pinned != a.mod:
        print(f"  pinned as {pinned.name} ({digest})")
    a.mod = pinned

    coins = coin_cfg(list(a.cfg))
    if coins is None:
        print(f"refused: --cfg names only one of {' / '.join(COIN_KEYS)}; a coin run "
              f"needs both, a coin-off run neither")
        return 2
    base = load_baseline(coins)
    if coins:
        if not base:
            print(f"no coin baseline yet ({BASELINE_COINS}): nothing is compared -- "
                  f"the coin-off baseline is not a stand-in for it")
        elif base.get("_meta", {}).get("cfg") != " ".join(COIN_KEYS):
            print(f"refused: {BASELINE_COINS.name} was blessed with cfg "
                  f"{base.get('_meta', {}).get('cfg')!r}, this run has "
                  f"{' '.join(COIN_KEYS)!r}")
            return 2

    # The profile's resolution is part of what a run measures (GD's saw radius
    # follows it; see gdsave.resolution_index), so it is printed, written into a
    # blessed baseline, and a baseline measured at another one is not compared.
    used = [a.pool[0]] if (a.one_session or a.bless) else list(a.pool)
    a.resolution, why = run_resolution(used)
    if why:
        print(f"refused: {why}")
        return 2
    if a.resolution is None:
        # Fail closed: a run that cannot say where it was measured is neither
        # compared nor blessed.
        print(f"refused: worker {','.join(str(w) for w in used)} has a hand-configured "
              f"profile without a resolution key, so this run could not say what it measured")
        return 2
    print(f"  resolution index {a.resolution} (worker {','.join(str(w) for w in used)})")
    a.geode, why = run_geode(used)
    if why:
        print(f"refused: {why}")
        return 2
    print(f"  Geode {a.geode['version']} (Geode.dll {str(a.geode['dll_sha256'])[:12]}, "
          f"worker {','.join(str(w) for w in used)})")
    bres = base.get("_meta", {}).get("resolution")
    if base and bres is None:
        print("  the baseline does not record the resolution it was measured at")
    elif base and bres != a.resolution:
        print(f"refused: the baseline was measured at resolution {bres}, these "
              f"workers play at {a.resolution}")
        return 2

    # The baseline is only worth what the arrangement that produced it can
    # disprove, so blessing runs the levels in one game. Nothing stops a plain
    # --one-session run from being used for anything else.
    one_session_mode = a.one_session or a.bless
    if a.bless and not a.one_session:
        print("--bless runs --one-session (the numbers have to come from an "
              "arrangement in which the mod cleans up between levels)")
    # The level deadline is read from the previous run's manifest, so before start()
    # replaces it.
    guard_settings = setup_guard(a, DATA / cold_manifest.NAME,
                                 (a.budget if a.budget else 3600.0) if one_session_mode
                                 else (a.budget if a.budget else 1200.0 * a.budget_scale))
    # What this run is, in data/cold_manifest.json beside the logs (py/cold_manifest.py):
    # compare two runs with `cold_manifest.py compare`, not by what their folders are called.
    cold_manifest.start(DATA, mod=a.mod, cfg=CFG + list(a.cfg), levels=list(a.levels),
                        arrangement="one-session" if one_session_mode else "per-level",
                        resolution=a.resolution, coins=coins,
                        build=getattr(a, "build_prov", None), geode=a.geode,
                        fresh_verify=bool(a.fresh_verify))
    run_guard.note_guard(DATA, guard_settings)
    logs = {lv: DATA / f"coldlog_lv{lv}.txt" for lv in a.levels}
    # A suite has no per-level clock to set, so its default stays where it was;
    # the parallel arrangement takes the 20-minute cap (times --budget-scale).
    per_level = a.budget if a.budget else 1200.0 * a.budget_scale
    if one_session_mode:
        wid = a.pool[0]
        # --budget is per level; a suite spends it end to end. It is a cap, not a
        # schedule -- the run ends when the mod says `suite: done`.
        budget = (a.budget if a.budget else 3600.0) * len(a.levels)
        print(f"  worker {wid}: {','.join(str(l) for l in a.levels)} "
              f"in ONE game (cap {budget / 3600:.1f} h)")
        # ...and what produced it, beside the log, so the run can be reviewed and
        # its baseline adopted later with --adopt instead of being run again.
        # TAKEN BEFORE THE RUN: the worker copies build/'s package at launch, so
        # a build that lands while the suite is running does not change what is
        # being measured -- but it does change what a hash taken afterwards
        # reads. On 2026-09-23 a mid-run build left a frozen log claiming a
        # package the run never loaded, and --adopt believes this file.
        a.run_meta = _run_meta(a)
        results = one_session(a.levels, wid, budget, list(a.cfg), DATA, a.mod)
        if a.fresh_verify_one_session:
            fresh_verify_one_session(results, wid, a.mod, DATA)
        elif a.fresh_verify:
            fresh_verify(results, wid, a.mod, DATA)
        (DATA / "coldlog_suite.meta.json").write_text(
            json.dumps(a.run_meta, indent=1, sort_keys=True), encoding="utf-8")
        for r in results:
            r["cap"] = iter_cap(r["lv"], base)
            print(f"lv{r['lv']:<3} {r['why']:<40} iters={r['iters']:<4}"
                  f"deepest t={r['deepest_t']:<6} x={r['deepest_x']:<9.0f}"
                  f"fx={r['fx']:<4} record={r['record']}", flush=True)
        cold_manifest.finish(DATA, logs)
        return report(results, base, a)

    buckets = assign(a.levels, a.pool)
    print(f"  per-level cap {per_level:.0f}s"
          + (f" (x{a.budget_scale:g})" if a.budget_scale != 1.0 else "")
          + ("" if a.budget else " -- the default; --budget overrides"))
    for w in a.pool:
        if buckets[w]:
            mins = sum(COST.get(l, 1.0) for l in buckets[w])
            print(f"  worker {w}: {','.join(str(l) for l in buckets[w])} "
                  f"(~{mins:.0f} min)")

    lock = threading.Lock()
    launched = [0]
    results: list[dict] = []

    def run_bucket(w: int) -> list[dict]:
        out = []
        with lock:
            k = launched[0]
            launched[0] += 1
        # Simultaneous GD starts are the documented worker trap; stagger the
        # first wave only.
        if k:
            time.sleep(k * 12.0)
        for lv in buckets[w]:
            cap = iter_cap(lv, base)
            res = one(lv, w, per_level, list(a.cfg), DATA, a.mod)
            if a.fresh_verify:
                fresh_verify([res], w, a.mod, DATA)
            res["cap"] = cap
            res["wallcap"] = per_level
            with lock:
                print(f"lv{lv:<3} {res['why']:<40} iters={res['iters']:<4}"
                      f"deepest t={res['deepest_t']:<6} x={res['deepest_x']:<9.0f}"
                      f"fx={res['fx']:<4} {res['wall']:>5.0f}s "
                      f"record={res['record']}", flush=True)
            out.append(res)
        return out

    busy = [w for w in a.pool if buckets[w]]
    with ThreadPoolExecutor(max_workers=max(1, len(busy))) as ex:
        for got in ex.map(run_bucket, busy):
            results.extend(got)

    cold_manifest.finish(DATA, logs)
    return report(results, base, a)


if __name__ == "__main__":
    sys.exit(main())
