"""Run the same plan in the model (leveldp) and in GD; print THE FIRST DIVERGING TICK.

    python py/fidelity_diff.py --levels 1 2 3 --pool 91 93 94

Finding out "where the model and GD disagree" does not require re-solving the
level. data/solution_lv*_dp.txt is verified to clear, so it is enough to REPLAY
the same input sequence on both sides and compare them (model replay ~0.1 s /
GD replay ~a few seconds).

Assumptions and etiquette:
  - by default `--fixups` is not passed to the model. A fixup is a sticking
    plaster that "overwrites the divergence with GD", so passing them erases
    the very divergence we want to measure. --with-fixups allows comparing.
  - the GD side is the same bare replay as verify_solutions (solve=0
    blockinput=1), except that NO notrace=1 IS SET (dump.csv is needed).
  - the comparison uses gdmcp.data.diff_trace as it is (do not build a second
    implementation).
  - on levels with moving geometry the model looks at static geometry unless
    `--groups` is given, so the result becomes "nothing but divergences" and is
    meaningless. Levels with no existing recording are reported as NO-GROUPS,
    their result treated as invalid rather than quietly passed.
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
from dataclasses import dataclass, field
from pathlib import Path

_PY = Path(__file__).resolve().parent
sys.path.insert(0, str(_PY))
sys.path.insert(0, str(_PY.parent / "mcp"))     # where gdmcp.data.diff_trace lives

from gdtas.solveutil import copy_held_file, has_grouped_colliders
from gdmcp.data import diff_trace
from gdtas import plan as P
from gdtas import results as R
from gdtas.paths import DATA, LEVEL_DATA, LEVELDP_EXE, WORKERS_ROOT

CREATE_NO_WINDOW = 0x08000000


def busy_worker_ids() -> set[int]:
    """Worker IDs where GD is running now. Two on one data root mix their results."""
    out = subprocess.run(["tasklist", "/NH", "/FO", "CSV"], capture_output=True,
                         text=True, errors="replace",
                         creationflags=CREATE_NO_WINDOW).stdout
    return {int(m) for m in re.findall(r"GeometryDash-worker-(\d+)\.exe", out)}


@dataclass
class Result:
    level: int
    status: str = "OK"            # OK | NO-GROUPS | ERROR
    note: str = ""
    first_t: int | None = None
    first_x: float = 0.0
    mode: str = ""
    dy: float = 0.0
    dvy: float = 0.0
    n_div: int = 0
    model_ticks: int = 0
    gd_ticks: int = 0
    model_died: int = -1
    gd_clear: bool = False
    gd_detail: str = ""
    cut_t: int | None = None      # GD-side comparison cut-off tick (frozen at goal)
    worker: int = 0
    seconds: float = 0.0
    rows: list = field(default_factory=list)

    def as_dict(self) -> dict:
        d = {k: v for k, v in self.__dict__.items() if k != "rows"}
        d["first_rows"] = self.rows[:8]
        return d


def groups_args(plan_out: Path) -> list[str]:
    """Order of --groups (= overwrite order). base -> deep -> live -> bank, bank last.

    deep (nodeath) comes BEFORE live: nodeath runs a different worldline from
    the plan (see the note on the deep recording overlay), so it is put on
    the side that gets overwritten by the recording of the real replay.
    """
    a = []
    for suffix in (".groups.txt", ".groups.deep.txt", ".groups.live.txt",
                   ".groups.bank.txt"):
        p = Path(str(plan_out) + suffix)
        if p.exists() and p.stat().st_size > 0:
            a += ["--groups", str(p)]
    return a


def model_replay(level: int, plan_out: Path, out_base: Path, leveldp: Path,
                 with_fixups: bool) -> tuple[Path, int, str]:
    """Replay the plan with leveldp to build .trace.csv. (trace, tick of death, raw log)."""
    objrects = LEVEL_DATA / f"objrects_lv{level}.txt"
    args = [str(objrects), "--replay", str(plan_out), "--out", str(out_base)]
    trig, grp = LEVEL_DATA / f"triggers_lv{level}.txt", LEVEL_DATA / f"objgroups_lv{level}.txt"
    if trig.exists() and grp.exists():
        args += ["--triggers", str(trig), "--objgroups", str(grp)]
    obb = LEVEL_DATA / f"obb_lv{level}.txt"
    if obb.exists():
        args += ["--obb", str(obb)]
    args += groups_args(plan_out)
    fx = Path(str(plan_out) + ".fixups.txt")
    if with_fixups and fx.exists():
        args += ["--fixups", str(fx)]
    r = subprocess.run([str(leveldp)] + args, stdout=subprocess.PIPE, text=True,
                       errors="replace")
    m = re.search(r"REPLAY: model DIED at t=(\d+)", r.stdout)
    return Path(str(out_base) + ".trace.csv"), (int(m.group(1)) if m else -1), r.stdout


def gd_replay(worker_id: int, level: int, plan_out: Path, dump_dst: Path,
              timeout_s: float, workers_root: Path) -> tuple[bool, str, float]:
    """Same bare replay as verify_solutions, but keeps dump.csv (notrace is dropped)."""
    from gdtas.worker import run_session
    cfg = [
        "enabled=1", f"level={level}", "attempts=1", "quitwhendone=1",
        "blockinput=1", "cbs=0", "cos=1",
        "fastdt=0.0166667", "fastloops=1800", "skiprender=1",
        "solve=0", "music=mute", "coins=0",
    ] + P.read_input_lines(plan_out)
    res = run_session(worker_id, cfg, timeout_s=timeout_s, workers_root=workers_root)
    clear, detail = R.clear_verdict(res.lines)
    if res.timed_out and not clear:
        detail = f"TIMEOUT ({detail})"
    if not copy_held_file(res.data_root / "dump.csv", dump_dst):
        detail = (detail + " | dump snapshot failed").strip(" |")
    goal_x = 0.0
    for l in res.lines:
        if l.startswith(R.COMPLETE) and "goalX=" in l:
            goal_x = R.parse_complete(l).get("goal_x") or 0.0
            break
    return clear, detail, goal_x


def gd_cut_tick(dump: Path, goal_x: float) -> int | None:
    """Tick at which the GD-side comparison is cut off. None if there is none.

    When the level is completed GD keeps writing the dump WITH THE PLAYER FROZEN
    at the goal (in lv1, from t=20329 to 21592 the same x,y,vy). The model flies
    on past the goal, so without the cut the "divergences" are padded out by
    hundreds of ticks (measured: lv1/2/3/8 all came out at 277). Cut at the goal
    if we know it was reached, otherwise at the frozen stretch at the end.
    """
    rows = []
    try:
        with dump.open(newline="", encoding="utf-8-sig", errors="replace") as f:
            for rec in csv.DictReader(f):
                try:
                    rows.append((int(rec["tick"]), float(rec["x"])))
                except (KeyError, ValueError, TypeError):
                    continue
    except OSError:
        return None
    if not rows:
        return None
    rows.sort()
    if goal_x > 0:
        for t, x in rows:
            if x >= goal_x:
                return t
    last_x = rows[-1][1]
    cut = rows[-1][0]
    for t, x in reversed(rows):
        if x != last_x:
            break
        cut = t
    return cut if cut < rows[-1][0] else None


def run_level(level: int, worker_id: int, a) -> Result:
    t0 = time.time()
    out = Result(level=level, worker=worker_id)
    plan_out = Path(a.plans.replace("{}", str(level)))
    if not plan_out.exists():
        out.status, out.note = "ERROR", f"no plan: {plan_out.name}"
        return out
    objrects = LEVEL_DATA / f"objrects_lv{level}.txt"
    if not objrects.exists():
        out.status, out.note = "ERROR", f"no objrects_lv{level}.txt"
        return out
    # if there is moving geometry but no time axis, the model replays against
    # static walls. That diff is not a "divergence" but an artefact of the
    # measurement rig, so drop it without printing numbers.
    if has_grouped_colliders(objrects) and not groups_args(plan_out):
        out.status = "NO-GROUPS"
        out.note = (f"no {plan_out.name}.groups*.txt. Without the time axis of "
                    "the moving geometry the diff means nothing, so it is not "
                    "measured (make one with py/regen_groups.py)")
        return out

    tmp = Path(a.tmp)
    tmp.mkdir(parents=True, exist_ok=True)
    base = tmp / f"fid_lv{level}"
    dump_dst = tmp / f"fid_lv{level}.dump.csv"
    try:
        trace, died, _ = model_replay(level, plan_out, base, Path(a.leveldp),
                                      a.with_fixups)
        out.model_died = died
        out.gd_clear, out.gd_detail, goal_x = gd_replay(
            worker_id, level, plan_out, dump_dst, a.timeout_minutes * 60,
            Path(a.workers_root))
        out.cut_t = gd_cut_tick(dump_dst, goal_x)
        d = diff_trace(trace, dump_dst, t1=out.cut_t, tol=a.tol, limit=10 ** 9)
    except Exception as e:                                   # noqa: BLE001
        out.status, out.note = "ERROR", f"{type(e).__name__}: {e}"
        out.seconds = time.time() - t0
        return out
    if "error" in d:
        out.status, out.note = "ERROR", d["error"]
        out.seconds = time.time() - t0
        return out
    out.model_ticks, out.gd_ticks = d["model_ticks"], d["gd_ticks"]
    rows = d["rows"]
    out.n_div = len(rows)
    out.rows = rows[:8]
    if rows:
        # the column order is diff_trace's cols:
        # tick,x,gd_y,gd_vy,gd_mode,vsize,m_y,m_vy,dy,dvy,dx,dy2,dvy2
        r0 = rows[0]
        out.first_t, out.first_x, out.mode = r0[0], r0[1], r0[4]
        out.dy, out.dvy = r0[8], r0[9]
    out.seconds = time.time() - t0
    return out


def format_table(results: list[Result], tol: float) -> str:
    hdr = (f"{'lv':<5}{'first div t=':<15}{'x=':<11}{'mode':<8}{'dy':<9}"
           f"{'dvy':<9}{'div ticks':<11}{'model/cut':<14}{'GD':<6}note")
    lines = [f"fidelity diff (tol={tol})", hdr, "-" * len(hdr)]
    for r in sorted(results, key=lambda r: r.level):
        gd = "clear" if r.gd_clear else "DEAD"
        span = f"{r.model_ticks}/{r.cut_t if r.cut_t is not None else r.gd_ticks}"
        if r.status != "OK":
            lines.append(f"lv{r.level:<3} {r.status:<14}{'':<45}{r.note}")
            continue
        note = r.note
        if r.model_died >= 0:
            note = (note + f" model died t={r.model_died}").strip()
        if r.first_t is None:
            lines.append(f"lv{r.level:<3}{'(no divergence)':<15}{'':<11}{'':<8}{'':<9}"
                         f"{'':<9}{0:<11}{span:<14}{gd:<6}{note}")
            continue
        lines.append(
            f"lv{r.level:<3}{r.first_t:<15}{r.first_x:<11.1f}{r.mode:<8}"
            f"{r.dy:<+9.3f}{r.dvy:<+9.3f}{r.n_div:<11}{span:<14}{gd:<6}{note}")
    lines.append("model/cut = ticks the model replayed / the tick the GD side is "
                 "cut at (GD freezes once the goal is reached, so it is cut there)")
    return "\n".join(lines)


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--levels", nargs="+", type=int, required=True)
    ap.add_argument("--pool", nargs="+", type=int, default=[91, 93, 94, 95, 96, 97])
    ap.add_argument("--plans", default=str(DATA / "solution_lv{}_dp.txt"),
                    help="path to the plan; {} is replaced by the level number")
    ap.add_argument("--tol", type=float, default=0.3)
    ap.add_argument("--with-fixups", action="store_true",
                    help="hand .fixups.txt to the model side, to see what is "
                         "left over once the patches are applied")
    ap.add_argument("--timeout-minutes", type=float, default=6.0)
    ap.add_argument("--leveldp", default=str(LEVELDP_EXE))
    ap.add_argument("--workers-root", default=str(WORKERS_ROOT))
    ap.add_argument("--tmp", default=str(Path(__file__).resolve().parents[1]
                                         / "build" / "fidelity"))
    ap.add_argument("--json", dest="json_out", default="")
    ap.add_argument("--allow-busy", action="store_true",
                    help="use a worker even if something else holds it "
                         "(result.txt then gets mixed up)")
    a = ap.parse_args(argv)

    busy = busy_worker_ids()
    pool = [w for w in a.pool if a.allow_busy or w not in busy]
    if busy:
        print(f"busy workers (skipped): {sorted(busy & set(a.pool)) or '-'} "
              f"/ all running: {sorted(busy)}")
    if not pool:
        print("no worker is free. Change --pool, or wait for the running GDs to finish.")
        return 2
    print(f"{len(a.levels)} levels on workers {pool}")

    # round-robin. One worker runs its own share in order (never use the same
    # worker in two sessions at once).
    buckets: dict[int, list[int]] = {w: [] for w in pool}
    for i, lv in enumerate(a.levels):
        buckets[pool[i % len(pool)]].append(lv)

    t_start = time.time()

    def run_bucket(w: int) -> list[Result]:
        return [run_level(lv, w, a) for lv in buckets[w]]

    results: list[Result] = []
    with ThreadPoolExecutor(max_workers=len(pool)) as ex:
        for chunk in ex.map(run_bucket, [w for w in pool if buckets[w]]):
            results.extend(chunk)
    elapsed = time.time() - t_start

    table = format_table(results, a.tol)
    print()
    print(table)
    print(f"--- {len(results)} levels in {elapsed:.1f}s "
          f"({'with' if a.with_fixups else 'without'} fixups) ---")
    if a.json_out:
        Path(a.json_out).write_text(json.dumps(
            {"tol": a.tol, "with_fixups": a.with_fixups, "seconds": elapsed,
             "levels": [r.as_dict() for r in sorted(results, key=lambda r: r.level)]},
            ensure_ascii=False, indent=1), encoding="utf-8")
        print(f"json -> {a.json_out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
