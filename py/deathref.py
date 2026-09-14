# -*- coding: utf-8 -*-
u"""DEATH REFERENCES: does the model still kill the player where GD does?

    python py/deathref.py --record          # build the references (needs GD workers)
    python py/deathref.py                   # check the model against them (seconds, no GD)
    python py/deathref.py --bless           # adopt the current ledger as the baseline
    python py/deathref.py --leveldp <exe>   # A/B a differential build against that ledger

## The hole this closes

`quick_regress` and `fixcensus` both measure the model against gdref, and gdref
is THE TRAJECTORY OF A VERIFIED SOLUTION -- i.e. a run that never dies. A change
that makes the model kill LESS than GD does therefore cannot be seen by either:
the corpus contains no death for the loosened rule to fail to reproduce. The
same asymmetry the campaign wrote down on 2026-09-01: over-kill is invisible to
the loop, under-kill shows up only as a fixup spike after a 75-minute cold run.

So this builds the missing half of the corpus. A verified solution is truncated
at a tick, the truncated input is replayed in real GD, and THE RUN THAT DIES IS
KEPT -- trajectory, death tick and the object GD names as the killer. Nothing
about the level is invented: the reference is a real death of a real replay.

The check then anchors the model a few hundred ticks in front of that death
(the same `--start` mechanism quick_regress's sections use), replays the same
truncated input, and asks the one question the rest of the pyramid cannot:
DOES THE MODEL DIE ON THE SAME TICK. A model that sails through is a kill gap,
and it is named in seconds instead of in a cold run.

## Reading a FAIL

A failure is not automatically a bug. The ledger separates two shapes, because
they have different owners:

  * `SURVIVES  (tracked)` -- the model matched GD tick for tick all the way to
    the death and then did not die. THAT IS A MISSING KILL, nothing else.
  * `... div t=N` -- the trajectories had already parted at N. The death tick
    then means little; the physics divergence is the thing to fix, and
    fixcensus is the instrument for it.

On current main several references fail. That is the finding this was built to
produce, not a defect of the harness: `--bless` writes them down so that the
next change is measured against the known state.

## Traps paid for once

  * A 1-line trace is an instant death, not agreement (2026-09-01, the anchored
    replay of lv20). Every trace here is counted, and the row count travels in
    the index.
  * The truncated plan gets the FULL solution's `.groups*.txt`. Moving geometry
    is driven by triggers that fire on x crossings, and x under forced scroll
    barely depends on the input, so the recording still describes the world for
    the few hundred ticks between the truncation and the death. It is a
    reference recorded for a different worldline all the same -- which is why a
    reference that dies far behind its truncation point is worth less than one
    that dies right after it (the index carries `death-trunc`).
  * `quick_regress.band_track_args` writes `gdref/lvN.bandtrack.txt`, which the
    whole suite reads. Rebuilding it from a death trace would poison every
    other instrument, so the band track of a reference is written next to that
    reference and nowhere else.
  * The GD side is a disposable session per truncation (`attempts=1
    quitwhendone=1`), exactly as gdref is recorded. A resident servemode worker
    would be much faster, but bands and stair snaps carry across attempts
    (gd-attempt-carryover), so run 2 would not equal run 1.
  * The killer's name costs nothing here: `gatetrace=<empty x window>` arms the
    `killer:` line in destroyPlayer while leaving its own recording switched off
    (no object falls in the window). `hitboxtrace=1` would arm the same line and
    also emit `pobb:` on EVERY tick -- 40,000 lines a level.
"""

from __future__ import annotations

import argparse
import csv
import json
import re
import subprocess
import sys
import time
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

_PY = Path(__file__).resolve().parent
sys.path.insert(0, str(_PY))
sys.path.insert(0, str(_PY.parent / "mcp"))

from gdmcp.data import diff_trace                                  # noqa: E402
from gdtas import plan as P                                        # noqa: E402
from gdtas import results as R                                     # noqa: E402
from gdtas.paths import DATA, LEVEL_DATA, LEVELDP_EXE, WORKERS_ROOT  # noqa: E402
from gdtas.solveutil import (copy_held_file, has_grouped_colliders,  # noqa: E402
                             rot2900s, spent2900_from_dump)
from gdtas.worker import run_session                               # noqa: E402
from fidelity_diff import busy_worker_ids, groups_args             # noqa: E402
from quick_regress import (REF, REF_COLS, pad_anchor_args,         # noqa: E402
                           start_fields)

DEATHS = REF / "deaths"
INDEX = DEATHS / "index.json"
BASELINE = DEATHS / "baseline.json"

# The GD side of a truncated replay. The same bare replay verify_solutions and
# gdref use (notrace is NOT set -- dump.csv is the reference), plus the empty
# gatetrace window that arms the `killer:` line. Do not add `nodeath`: the whole
# point is that the run dies.
REPLAY_CFG = ["enabled=1", "attempts=1", "quitwhendone=1", "blockinput=1",
              "cbs=0", "cos=1", "fastdt=0.0166667", "fastloops=1800",
              "skiprender=1", "solve=0", "music=mute", "coins=0",
              "gatetrace=-2,-1"]


# ---------------------------------------------------------------- references

def csv_path(level: int, trunc: int) -> Path:
    return DEATHS / f"lv{level}_t{trunc}.csv"


def plan_path(level: int, trunc: int) -> Path:
    return DEATHS / f"lv{level}_t{trunc}.plan.txt"


def band_path(level: int, trunc: int) -> Path:
    return DEATHS / f"lv{level}_t{trunc}.bandtrack.txt"


def ref_key(level: int, trunc: int) -> str:
    return f"lv{level}_t{trunc}"


def rows_of(path: Path) -> dict[int, dict]:
    """Read a reference trace as {tick: row}. Same shape as quick_regress.read_ref."""
    out: dict[int, dict] = {}
    if not path.exists():
        return out
    with path.open(newline="", encoding="utf-8-sig", errors="replace") as f:
        for rec in csv.DictReader(f):
            try:
                out[int(rec["tick"])] = rec
            except (KeyError, ValueError, TypeError):
                continue
    return out


def frozen_from(src: Path) -> int | None:
    u"""The tick GD's dump stops moving on, i.e. THE TICK THE PLAYER DIED ON.

    GD's own `death:` line reports g_tick at destroyPlayer, and that is ONE
    BEHIND the dump row holding the dead state: the row is written at the top of
    the frame, so row N carries the result of the frame whose g_tick was N-1.
    Measured on lv15 t=9,632: the death line says 9,825, the dump moves for the
    last time at 9,826 and repeats that row to the end of the session, and the
    model (whose trace rows line up with the dump's ticks -- that is what the
    whole of quick_regress rests on) dies at 9,826.

    Deriving it from the trace rather than adding 1 to the death line keeps the
    harness honest if that offset is ever not 1. None means the dump never
    settles, and then the caller falls back on the death line.
    """
    rows: list[tuple[int, str, str, str]] = []
    with src.open(newline="", encoding="utf-8-sig", errors="replace") as f:
        for rec in csv.DictReader(f):
            try:
                rows.append((int(rec["tick"]), rec["x"], rec["y"], rec["yvel"]))
            except (KeyError, ValueError, TypeError):
                continue
    if len(rows) < 3:
        return None
    i = len(rows) - 1
    while i > 0 and rows[i - 1][1:] == rows[-1][1:]:
        i -= 1
    return rows[i][0] if len(rows) - i >= 2 else None


def write_ref(src: Path, dst: Path, cut: int | None) -> int:
    """Trim GD's dump to the reference columns, up to and including `cut`.

    The frozen tail after the death (4,567 rows on the lv15 sample) is dropped:
    it is the same row over and over, and a reader that mistook it for real
    states would see the model "diverge" for thousands of ticks after a death
    both sides agree on.
    """
    n = 0
    dst.parent.mkdir(parents=True, exist_ok=True)
    with src.open(newline="", encoding="utf-8-sig", errors="replace") as f, \
            dst.open("w", newline="", encoding="utf-8") as g:
        rd = csv.DictReader(f)
        w = csv.DictWriter(g, fieldnames=REF_COLS, extrasaction="ignore")
        w.writeheader()
        for rec in rd:
            try:
                if cut is not None and int(rec["tick"]) > cut:
                    break
            except (KeyError, ValueError, TypeError):
                continue
            w.writerow({k: rec.get(k, "") for k in REF_COLS})
            n += 1
    return n


def truncation_ticks(cut: int, points: int) -> list[int]:
    """`points` evenly spaced truncation ticks inside the run.

    i/(points+1) of the run, so neither end is included: a truncation at tick 0
    is not a death of the level, and one past the last input is not a truncation
    at all.
    """
    return [int(round(cut * i / (points + 1))) for i in range(1, points + 1)]


# --------------------------------------------------------------- record mode

def record_one(level: int, trunc: int, worker_id: int, a) -> dict:
    """Replay one truncation in GD and, if it dies, write the reference."""
    out = {"level": level, "trunc": trunc, "worker": worker_id,
           "status": "ERROR", "note": ""}
    full = Path(a.plans.replace("{}", str(level)))
    if not full.exists():
        out["note"] = f"no solution {full.name}"
        return out
    inputs = [e for e in P.read_inputs(full) if e[0] <= trunc]
    if not inputs:
        out["note"] = f"no input at or before t={trunc}"
        return out
    pp = plan_path(level, trunc)
    pp.parent.mkdir(parents=True, exist_ok=True)
    P.write_plan(pp, P.format_plan(inputs))

    cfg = REPLAY_CFG + [f"level={level}"] + [f"input={t},{d}" for t, d in inputs]
    t_start = time.time()
    # RETRY A STALLED LAUNCH ONCE. Two of the first 88 recordings came back with
    # `session-stall: no result.txt update for 90s` and produced no verdict at
    # all; re-run, both died normally. Left unretried, an environment hiccup
    # sends the level into the widening round and A DIFFERENT SET OF REFERENCES
    # IS KEPT -- i.e. the recording stops being reproducible for a reason that
    # has nothing to do with the game.
    for attempt in range(2):
        res = run_session(worker_id, cfg, timeout_s=a.timeout_minutes * 60,
                          workers_root=Path(a.workers_root))
        lines = res.lines
        death = None
        for l in lines:
            if l.startswith(R.DEATH):
                death = R.parse_death(l)
                break
        if death is not None:
            break
        if any(l.startswith(R.COMPLETE) for l in lines):
            out["status"] = "NO-DEATH"
            out["note"] = "the truncated plan still completes"
            return out
        if not res.timed_out or attempt:
            out["status"] = "NO-DEATH"
            out["note"] = ("STALLED twice, no verdict" if res.timed_out
                           else "no death:/complete: line")
            return out

    tmp = Path(a.tmp)
    tmp.mkdir(parents=True, exist_ok=True)
    dump = tmp / f"dr_lv{level}_t{trunc}.dump.csv"
    if not copy_held_file(res.data_root / "dump.csv", dump):
        out["note"] = "dump.csv could not be copied"
        return out
    frozen = frozen_from(dump)
    expect = frozen if frozen is not None else death["tick"] + 1
    rows = write_ref(dump, csv_path(level, trunc), expect)
    gd = rows_of(csv_path(level, trunc))
    # A 1-line (or empty) trace is an instant death, not a reference.
    if rows < a.min_rows or not gd or max(gd) != expect:
        out["status"] = "SHORT"
        out["note"] = (f"only {rows} rows"
                       if max(gd or {0: 0}) == expect
                       else f"{rows} rows, ends at {max(gd or {0: 0})} not {expect}")
        return out

    killer = ""
    for l in lines:
        if l.startswith(R.KILLER):
            kv = R.parse_killer(l)
            killer = (f"uid{kv.get('uid', '?')} id{kv.get('id', '?')} "
                      f"type{kv.get('type', '?')} @{kv.get('ox', '?')},"
                      f"{kv.get('oy', '?')}") if "uid" in kv else l[len(R.KILLER):].strip()
    out.update({
        "status": "OK", "death_tick": death["tick"], "death_x": death["x"],
        # the tick the MODEL has to die on (see frozen_from). `death_tick` stays
        # in the index as GD said it, so the two clocks can always be compared.
        "expect": expect, "frozen": frozen is not None,
        "speed_x": death["speed_x"], "rows": rows, "last_tick": max(gd),
        "killer": killer, "seconds": round(time.time() - t_start, 1),
        "csv": csv_path(level, trunc).name, "plan": plan_path(level, trunc).name,
    })
    return out


def dedupe(found: list[dict], px: float) -> tuple[list[dict], list[dict]]:
    """Keep one reference per death site. Returns (kept, dropped).

    Two truncations that die at the same x exercise the same geometry, and a
    ledger padded with copies of one death would report detection power it does
    not have. Sites are x buckets, and the earliest truncation wins so that the
    kept set does not depend on the order the workers happened to finish in.
    """
    kept: dict[int, dict] = {}
    dropped: list[dict] = []
    for d in sorted(found, key=lambda d: d["trunc"]):
        site = int(round(d["death_x"] / px))
        if site in kept:
            dropped.append(dict(d, dup_of=ref_key(d["level"], kept[site]["trunc"])))
            continue
        kept[site] = d
    return [kept[k] for k in sorted(kept, key=lambda k: kept[k]["trunc"])], dropped


def spread(kept: list[dict], target: int) -> tuple[list[dict], list[dict]]:
    """Thin a surplus of references down to `target`, EVENLY ALONG THE LEVEL.

    Taking the first N instead put every lv17 and lv21 reference in the first
    half of the level: the widening round adds truncations everywhere, so
    "earliest wins" collapses the coverage exactly on the levels that needed
    widening. The deaths are ordered by truncation tick, which is monotone in
    position, so an even subsample of that order is an even subsample of the
    level.
    """
    if len(kept) <= target or target < 1:
        return kept, []
    n = len(kept)
    idx = sorted({0} if target == 1 else
                 {int(round(i * (n - 1) / (target - 1))) for i in range(target)})
    return ([kept[i] for i in idx],
            [dict(kept[i], dup_of="even spread") for i in range(n) if i not in idx])


def run_pool(level: int, ticks: list[int], a) -> list[dict]:
    """Run these truncations on the pool, ONE WORKER PER BUCKET.

    Not round-robin over a thread pool: with more truncations than workers a
    later item can be scheduled onto a worker id whose earlier item has not
    finished, and two GDs on one data root mix up result.txt. Each worker gets
    its own list and walks it in order.
    """
    buckets: dict[int, list[int]] = {w: [] for w in a.pool}
    for i, t in enumerate(ticks):
        buckets[a.pool[i % len(a.pool)]].append(t)

    def one(w: int) -> list[dict]:
        out = []
        for t in buckets[w]:
            try:
                out.append(record_one(level, t, w, a))
            except Exception as e:                              # noqa: BLE001
                out.append({"level": level, "trunc": t, "worker": w,
                            "status": "ERROR", "note": f"{type(e).__name__}: {e}"})
        return out

    res: list[dict] = []
    live = [w for w in a.pool if buckets[w]]
    with ThreadPoolExecutor(max_workers=len(live)) as ex:
        for chunk in ex.map(one, live):
            res += chunk
    return res


def record(a) -> int:
    # Only OUR pool matters: worker 98/99 belong to the resident MCP session and
    # a Wine container's GD does not appear here at all. A worker of the
    # mainline batch pool (90-97) that is not in our pool is still worth saying
    # out loud -- it means a cold run is in progress.
    busy = busy_worker_ids()
    if busy:
        print(f"GD running on workers {sorted(busy)}")
    clash = busy & set(a.pool)
    if clash and not a.allow_busy:
        print(f"workers {sorted(clash)} are busy. Recording on them would share "
              f"a data root with a live GD (--allow-busy to override).")
        return 2
    DEATHS.mkdir(parents=True, exist_ok=True)
    cuts = json.loads((REF / "cut.json").read_text()) \
        if (REF / "cut.json").exists() else {}
    index = json.loads(INDEX.read_text(encoding="utf-8")) if INDEX.exists() else {}
    t_start = time.time()

    if a.trunc:
        # AIMED references. The evenly spaced sweep finds whatever the level
        # happens to kill with; a gap that is suspected in advance (a kill arm
        # behind a gate, say) needs the truncation put where it can fire. The
        # level's existing references are kept and these are added to them.
        if len(a.levels) != 1:
            print("--trunc names ticks of ONE level; pass --levels <n>")
            return 2
        lv = a.levels[0]
        for r in sorted(run_pool(lv, sorted(set(a.trunc)), a),
                        key=lambda r: r["trunc"]):
            tag = (f"death t={r['expect']} x={r['death_x']:.0f} "
                   f"(+{r['expect'] - r['trunc']}) rows={r['rows']} "
                   f"{r['killer']}") if r["status"] == "OK" else r["note"]
            print(f"  lv{lv} t={r['trunc']:<6} aimed {r['status']:<9}{tag}")
            if r["status"] == "OK":
                r["round"] = 0
                index[ref_key(lv, r["trunc"])] = r
        INDEX.write_text(json.dumps(index, indent=1, sort_keys=True),
                         encoding="utf-8")
        print(f"--- {len(index)} references in {DEATHS} "
              f"({time.time() - t_start:.1f}s) ---")
        return 0

    for lv in a.levels:
        full = Path(a.plans.replace("{}", str(lv)))
        if has_grouped_colliders(LEVEL_DATA / f"objrects_lv{lv}.txt") \
                and not groups_args(full):
            # gd-missing-groups-silent-skip: say it out loud.
            print(f"lv{lv}: NO GROUPS ({full.name}.groups*.txt missing) -- the "
                  f"model would replay against static walls. Skipped.")
            continue
        cut = int(cuts.get(str(lv)) or 0)
        if not cut:
            print(f"lv{lv}: no cut.json entry (run quick_regress --record first)")
            continue
        # this level's references are being rebuilt, so its old files go first
        # (a --points change otherwise leaves orphans that no index names)
        for p in DEATHS.glob(f"lv{lv}_t*"):
            p.unlink()
        rounds = [truncation_ticks(cut, a.points)]
        if a.widen:
            rounds.append(truncation_ticks(cut, a.widen))
        seen: set[int] = set()
        found: list[dict] = []
        for rnd, ticks in enumerate(rounds, 1):
            todo = [t for t in ticks if t not in seen]
            seen.update(todo)
            if not todo:
                continue
            res = run_pool(lv, todo, a)
            for r in sorted(res, key=lambda r: r["trunc"]):
                tag = (f"death t={r['expect']} x={r['death_x']:.0f} "
                       f"(+{r['expect'] - r['trunc']}) rows={r['rows']} "
                       f"{r['killer']}") if r["status"] == "OK" else r["note"]
                print(f"  lv{lv} t={r['trunc']:<6} r{rnd} {r['status']:<9}{tag}")
                if r["status"] == "OK":
                    r["round"] = rnd
                    found.append(r)
            kept, _ = dedupe(found, a.dedupe_px)
            if len(kept) >= a.target:
                break
        kept, dropped = dedupe(found, a.dedupe_px)
        kept, thinned = spread(kept, a.target)
        dropped += thinned
        for d in sorted(dropped, key=lambda d: d["trunc"]):
            print(f"  lv{lv} t={d['trunc']:<6} dropped ({d['dup_of']})")
            csv_path(lv, d["trunc"]).unlink(missing_ok=True)
            plan_path(lv, d["trunc"]).unlink(missing_ok=True)
        # rebuild this level's slice of the index from what was just recorded
        index = {k: v for k, v in index.items() if v.get("level") != lv}
        for d in kept:
            index[ref_key(lv, d["trunc"])] = d
        INDEX.write_text(json.dumps(index, indent=1, sort_keys=True),
                         encoding="utf-8")
        if len(kept) < a.target:
            print(f"  lv{lv}: {len(kept)} distinct death sites, target "
                  f"{a.target} (SHORTFALL -- widen --points/--widen rather "
                  f"than padding with duplicates)")
        else:
            print(f"  lv{lv}: {len(kept)} references")

    print(f"--- {len(index)} references in {DEATHS} "
          f"({time.time() - t_start:.1f}s) ---")
    return 0


# ---------------------------------------------------------------- check mode

def anchor_tick(gd: dict[int, dict], death: int, window: int) -> int | None:
    """The tick the model is anchored at: `window` ticks in front of the death.

    Walk backwards until a row and its predecessor both exist (start_fields
    reads the previous row for the rotation direction and the reverse trend).
    ANCHORING EARLIER IS THE SAFE DIRECTION -- the section only gets longer and
    the death stays inside it -- so a death closer to the start of the level
    than `window` anchors at the first usable row rather than reporting that it
    cannot be measured (lv18's t=267 death at tick 330 did exactly that).
    """
    lo = min(gd) + 1
    t = min(death - window, max(gd))
    while t >= lo:
        if t in gd and (t - 1) in gd:
            return t
        t -= 1
    for t in range(lo, min(death, max(gd)) + 1):
        if t in gd and (t - 1) in gd:
            return t
    return None


def band_args(level: int, trunc: int, gd: dict[int, dict]) -> list[str]:
    """Write this reference's own band track and point --bandtrack at it.

    Never `gdref/lvN.bandtrack.txt`: that file belongs to the verified solution
    and quick_regress / fixcensus read it.
    """
    dst = band_path(level, trunc)
    rows, prev = 0, None
    with dst.open("w", encoding="utf-8") as w:
        for t in sorted(gd):
            r = gd[t]
            lo, hi = r.get("pmin"), r.get("pmax")
            # Same four kinds as quick_regress's writer: a field we could not
            # read is U, a collapsed band is D, only an interval is I. `not lo`
            # would have dropped a real zero as quietly as a missing field.
            if lo in (None, "") or hi in (None, ""):
                cur = ("U", "", "")
            elif float(hi) > float(lo):
                cur = ("I", lo, hi)
            elif float(hi) == float(lo):
                cur = ("D", lo, lo)
            else:
                cur = ("U", "", "")
            if cur != prev:
                w.write(f"{t},{cur[0]},{cur[1]},{cur[2]}\n")
                rows += 1
            prev = cur
    return ["--bandtrack", str(dst)] if rows else []


def ctrlwin_args(gd: dict[int, dict]) -> list[str]:
    """`--ctrlwin` from this reference's own ctrlOff column (see quick_regress)."""
    wins, t0 = [], None
    for t in sorted(gd):
        on = gd[t].get("ctrlOff") == "1"
        if on and t0 is None:
            t0 = t
        elif not on and t0 is not None:
            wins.append((t0, t - 1))
            t0 = None
    if t0 is not None:
        wins.append((t0, max(gd)))
    return ["--ctrlwin", ",".join(f"{x}:{y}" for x, y in wins)] if wins else []


def check_one(ref: dict, a) -> dict:
    """Anchor the model in front of one recorded death and see whether it dies."""
    lv, trunc = ref["level"], ref["trunc"]
    # the dump's own clock, not GD's death line (frozen_from explains the two)
    expect = int(ref.get("expect") or ref["death_tick"] + 1)
    out = {"key": ref_key(lv, trunc), "level": lv, "trunc": trunc,
           "gd_died": expect, "status": "ERROR", "note": "",
           "model_died": -1, "firstdiv": None, "trace_rows": 0,
           "cause": "", "killer": ref.get("killer", "")}
    ref_csv, plan = csv_path(lv, trunc), plan_path(lv, trunc)
    if not ref_csv.exists() or not plan.exists():
        out["note"] = "reference files missing (--record)"
        return out
    gd = rows_of(ref_csv)
    if not gd:
        out["note"] = f"no rows in {ref_csv.name}"
        return out
    full = Path(a.plans.replace("{}", str(lv)))
    objrects = LEVEL_DATA / f"objrects_lv{lv}.txt"
    if has_grouped_colliders(objrects) and not groups_args(full):
        out["status"], out["note"] = "SKIP", f"no {full.name}.groups*.txt"
        return out

    # AN ANCHOR CAN LAND IN A STATE THE MODEL KILLS ON THE SPOT (lv18 t=16,079,
    # a death 2,700 px above the level: 500 ticks in front of it the player is
    # already out of play, the replay dies at the first tick and the 1-row trace
    # would read as "no divergence found" = perfect agreement). Back the anchor
    # off and try again; earlier is the safe direction. Deterministic, and the
    # anchor actually used travels in the result.
    for mult in (1, 2, 4):
        t0 = anchor_tick(gd, expect, a.window * mult)
        if t0 is None:
            out["note"] = "no anchorable row in front of the death"
            return out
        out["anchor"] = t0
        if run_model(out, lv, trunc, t0, gd, full, objrects, a) > 1:
            break
    return verdict_of(out, expect, a)


def run_model(out: dict, lv: int, trunc: int, t0: int, gd: dict, full: Path,
              objrects: Path, a) -> int:
    """Replay the truncated plan from the anchor. Fills out, returns the rows."""
    tmp = Path(a.tmp)
    tmp.mkdir(parents=True, exist_ok=True)
    base = tmp / f"dr_{ref_key(lv, trunc)}"
    plan, ref_csv = plan_path(lv, trunc), csv_path(lv, trunc)
    r0 = gd[t0]
    args = [str(objrects), "--replay", str(plan),
            "--start", start_fields(t0, r0, plan, gd.get(t0 - 1), gd),
            "--out", str(base)]
    if r0.get("pmin") and r0.get("pmax"):
        args += ["--startband", f"{r0['pmin']},{r0['pmax']}"]
    args += band_args(lv, trunc, gd)
    trig, grp = (LEVEL_DATA / f"triggers_lv{lv}.txt",
                 LEVEL_DATA / f"objgroups_lv{lv}.txt")
    if trig.exists() and grp.exists():
        args += ["--triggers", str(trig), "--objgroups", str(grp)]
    obb = LEVEL_DATA / f"obb_lv{lv}.txt"
    if obb.exists():
        args += ["--obb", str(obb)]
    # THE MOVING-GEOMETRY RECORDING COMES FROM THE FULL SOLUTION (see the header):
    # the truncated run has none of its own.
    args += groups_args(full)
    args += ctrlwin_args(gd)
    # the one-shot history of the gameplay-rotation triggers, from THIS
    # reference's own trace (lv22 only; every other level gets an empty list)
    rots = rot2900s(objrects)
    if rots:
        args += ["--trigraw"]
        spent = spent2900_from_dump(ref_csv, -1, t0, rots)
        if spent:
            args += ["--spentrot", ",".join(str(u) for u in spent)]
    # ...and the pads this reference's own run had fired before t0. THIS
    # reference's, not gdref's: a truncated plan follows the full one only up
    # to the cut, and the anchor sits past it. pad_anchor_args keys its cache
    # on the recording for exactly that reason.
    args += pad_anchor_args(lv, t0, gd)

    # stderr is folded into stdout, not discarded: leveldp reports a rejected
    # argument there (`startband: 90,90 is not a readable f,c pair` -- a band
    # whose floor and ceiling are equal, which the anchor simply does without),
    # and a harness that throws that away cannot tell a rejected argument from
    # an accepted one.
    r = subprocess.run([str(a.leveldp)] + args, stdout=subprocess.PIPE,
                       stderr=subprocess.STDOUT, text=True, errors="replace")
    m = re.search(r"REPLAY: model DIED at t=(\d+)", r.stdout)
    out["model_died"] = int(m.group(1)) if m else -1
    mc = re.search(r"REPLAY: cause=(\S+)", r.stdout)
    out["cause"] = mc.group(1) if mc else ""
    trace = Path(str(base) + ".trace.csv")
    # ALWAYS COUNT THE ROWS. A 1-row trace is an instant death at the anchor,
    # and read as "no divergence found" it looks exactly like perfect agreement.
    out["trace_rows"] = max(0, sum(1 for _ in trace.open(encoding="utf-8-sig",
                                                         errors="replace")) - 1) \
        if trace.exists() else 0
    out["firstdiv"] = None
    d = diff_trace(trace, ref_csv, t0=t0, t1=max(gd), tol=a.tol, limit=10 ** 9)
    if "error" not in d and d["rows"]:
        out["firstdiv"] = int(d["rows"][0][0])
    return out["trace_rows"]


def verdict_of(out: dict, expect: int, a) -> dict:
    """Turn the replay's answer into PASS / FAIL, and say which kind of FAIL.

    The two shapes have different owners: a model that tracked GD to the death
    and then did not die is a MISSING KILL, while one whose trajectory had
    already parted company is a physics divergence and fixcensus's business.
    """
    delta = out["model_died"] - expect
    if out["trace_rows"] <= 1:
        out["status"], out["note"] = "FAIL", f"trace has {out['trace_rows']} rows"
    elif out["model_died"] < 0:
        out["status"] = "FAIL"
        out["note"] = "SURVIVES" + (" (tracked)" if out["firstdiv"] is None
                                    else f" div t={out['firstdiv']}")
    elif abs(delta) <= a.slack:
        out["status"] = "PASS"
        out["note"] = "" if delta == 0 else f"{delta:+d} tick"
    else:
        out["status"] = "FAIL"
        out["note"] = f"died {delta:+d}" + ("" if out["firstdiv"] is None
                                            else f" div t={out['firstdiv']}")
    return out


def check(a) -> int:
    if not INDEX.exists():
        print(f"no references yet: {INDEX} (run --record)")
        return 2
    index = json.loads(INDEX.read_text(encoding="utf-8"))
    refs = [v for k, v in sorted(index.items())
            if v.get("level") in set(a.levels)]
    if not refs:
        print(f"no references for levels {a.levels}")
        return 2
    t_start = time.time()
    with ThreadPoolExecutor(max_workers=a.parallel) as ex:
        now = list(ex.map(lambda ref: check_one(ref, a), refs))
    now.sort(key=lambda r: (r["level"], r["trunc"]))
    return report(now, a, time.time() - t_start)


def report(now: list[dict], a, elapsed: float) -> int:
    base = json.loads(BASELINE.read_text(encoding="utf-8")) \
        if BASELINE.exists() else {}
    print(f"{'reference':<16}{'anchor':<9}{'GD dies':<9}{'model':<9}"
          f"{'rows':<7}{'verdict':<8}{'vs baseline':<20}note")
    regressed, npass = [], 0
    for r in now:
        b = base.get(r["key"])
        vs = ""
        if b:
            if b.get("status") == r["status"] and b.get("model_died") == r["model_died"]:
                vs = "same"
            else:
                vs = f"{b.get('status')}->{r['status']}"
                if b.get("status") == "PASS" and r["status"] != "PASS":
                    regressed.append(f"{r['key']}: {vs} ({r['note']})")
        if r["status"] == "PASS":
            npass += 1
        note = " ".join(x for x in (r["note"], r["cause"]) if x)
        print(f"{r['key']:<16}{r.get('anchor', '-'):<9}{r['gd_died']:<9}"
              f"{r['model_died']:<9}{r['trace_rows']:<7}{r['status']:<8}"
              f"{vs:<20}{note}")
    print(f"--- {npass}/{len(now)} PASS ({elapsed:.1f}s, "
          f"{Path(a.leveldp).name}) ---")
    if a.json_out:
        Path(a.json_out).write_text(
            json.dumps({"leveldp": str(a.leveldp), "seconds": elapsed,
                        "refs": now}, indent=1), encoding="utf-8")
        print(f"json -> {a.json_out}")

    rc = 0
    if base:
        if regressed:
            print("REGRESSED vs baseline:")
            for l in regressed:
                print("  " + l)
            rc = 1
        else:
            print("no reference that used to reproduce its death lost it")
    else:
        print("no baseline yet -- run with --bless to record this ledger")
    if a.bless:
        BASELINE.write_text(json.dumps(
            {r["key"]: {k: r[k] for k in
                        ("status", "model_died", "gd_died", "firstdiv", "cause")}
             for r in now}, indent=1, sort_keys=True), encoding="utf-8")
        print(f"baseline updated: {BASELINE} ({len(now)} references)")
        return 0
    return rc


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--levels", nargs="+", type=int,
                    default=list(range(15, 23)))
    ap.add_argument("--plans", default=str(DATA / "solution_lv{}_dp.txt"))
    ap.add_argument("--leveldp", default=str(LEVELDP_EXE))
    ap.add_argument("--tmp", default=str(DATA / "tmp_deathref"))
    ap.add_argument("--record", action="store_true",
                    help="replay truncated solutions in GD and keep the deaths")
    ap.add_argument("--bless", action="store_true",
                    help="save the current ledger as the baseline")
    ap.add_argument("--json", dest="json_out", default="")
    # --- record ---
    ap.add_argument("--trunc", nargs="*", type=int,
                    help="record exactly these truncation ticks, on ONE level "
                         "(--levels n). For aiming a reference at a suspected "
                         "gap; adds to that level's references instead of "
                         "replacing them")
    ap.add_argument("--points", type=int, default=5,
                    help="evenly spaced truncation ticks per level")
    ap.add_argument("--widen", type=int, default=11,
                    help="a second, finer round of truncations, run only while "
                         "the level is short of --target distinct death sites "
                         "(0 disables)")
    ap.add_argument("--target", type=int, default=5,
                    help="references kept per level")
    ap.add_argument("--dedupe-px", type=float, default=40.0,
                    help="deaths within this many px of x are the same site")
    ap.add_argument("--min-rows", type=int, default=200,
                    help="reject a reference whose trace is shorter than this")
    ap.add_argument("--pool", nargs="+", type=int, default=[90, 91, 92, 93, 94, 95])
    ap.add_argument("--workers-root", default=str(WORKERS_ROOT))
    ap.add_argument("--timeout-minutes", type=float, default=6.0)
    ap.add_argument("--allow-busy", action="store_true",
                    help="record even though GD is running on a pool worker")
    # --- check ---
    ap.add_argument("--window", type=int, default=500,
                    help="how far in front of the death the model is anchored")
    ap.add_argument("--slack", type=int, default=0,
                    help="tolerated difference in the death tick")
    ap.add_argument("--tol", type=float, default=0.3,
                    help="px tolerance of the divergence classifier")
    ap.add_argument("--parallel", type=int, default=8)
    a = ap.parse_args(argv)

    DEATHS.mkdir(parents=True, exist_ok=True)
    return record(a) if a.record else check(a)


if __name__ == "__main__":
    sys.exit(main())
