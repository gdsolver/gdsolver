"""Does a result from a game that was NOT relaunched equal the result of a fresh game?

The resident session (serverearm) and the in-game level switch exist to save relaunches. They are
worth having only if every observation they produce is the one a relaunch would have produced.
CLAUDE.md records why that cannot be assumed: one level's moving-geometry recording sat under a
green 22/22 for months, adopted by the next level, because no test ever ran two levels in one game.

So this runs the SAME plans twice and compares every row.

    arm F ("fresh"):    one new GD process per case, serverearm off -- how analysis worked before
    arm R ("resident"): ONE GD process, the cases in order, `switch` between levels and a reset in
                        place between plans on one level (including after a clear)

What is compared, per case: dump.csv and trace.csv (every row, every column but `frame` and
`attempt` -- the first is a wall-clock frame counter, the second is how many attempts the process
has made, and both are meant to differ), grouptrace_last.txt, and the run's result lines (outcome,
tick, x, pct, goal and the lines between the load marker and the verdict, with the counters that are
meant to differ masked). A difference names the case, the file, the row and the column.

    python -m gdmcp.equiv --worker 102 --levels 1 2 15 --out <a new directory>

Never run this on a worker another session is using; it kills the GD it starts.
"""

from __future__ import annotations

import argparse
import csv
import json
import re
import shutil
import sys
import time
from pathlib import Path

from . import tools as T
from . import worker as W
from gdtas.paths import REPO

# Observation switches. Every one of them adds rows or columns to what is compared, so a leak in
# any of those recorders is a difference rather than something nobody looked at.
RICH_CFG = {"grouptrace": "1", "trigtrace": "1", "orbtrace": "1", "padtrace": "1"}
IGNORED_COLS = {"frame", "attempt"}
# Counters inside result lines that legitimately differ between the arms.
MASKS = [(re.compile(r"attempt=\d+"), "attempt=N"), (re.compile(r"wallMs=\d+"), "wallMs=N"),
         (re.compile(r"speedX=[-\d.e+]+"), "speedX=N"),
         (re.compile(r"\(attempt \d+\)"), "(attempt N)"),
         (re.compile(r"\b\d+ ?ms\b"), "N ms"), (re.compile(r"\b\d+\.\d+ ?s\b"), "N s")]


def mask(line: str) -> str:
    for rx, rep in MASKS:
        line = rx.sub(rep, line)
    return line


def load_plan(path: Path) -> list[tuple[int, int]]:
    return T.parse_plan_file(path)


def make_cases(levels: list[int], plan_dir: Path, death_inputs: int) -> list[dict]:
    """For each level: the stored clear, then a plan that is cut short so the run dies. The
    clear goes first and last, so the resident arm is also checked when the same level is played
    again after a clear AND after a death."""
    cases = []
    for lv in levels:
        p = plan_dir / f"solution_lv{lv}_dp.txt"
        if not p.exists():
            print(f"[skip] no plan for level {lv}: {p}")
            continue
        full = load_plan(p)
        cases.append({"level": lv, "name": "clear", "plan": full})
        cases.append({"level": lv, "name": "death", "plan": full[:death_inputs]})
        cases.append({"level": lv, "name": "clear2", "plan": full})
    return cases


# The lines of a run's result that carry its outcome. The rest differ by construction: the arm
# that really ends the session prints its whole end-of-session report and GD's own end screen
# (`endscreen:`), the resident arm does not; `cmd_ack` quotes the tick the poll happened to see.
VERDICT_LINES = ("death:", "complete:", "stop:", "inject:", "killer:", "gt_last:",
                 "coin:", "coincmp:", "serve: loaded")
# (`clearcheck:` is not here either: the mod prints its first 40 per process, a counter that is a
# static of the function -- a diagnostic of how many clears the PROCESS has seen, not of the run.)
# (`endtrigger:` is not here: GD's own completion lets the end trigger fire one tick after
# `complete:`, and a worker that keeps the level does not run that tick.)


def verdict_lines(lines: list[str]) -> list[str]:
    return [mask(l) for l in lines if l.startswith(VERDICT_LINES)]


def snap(worker, case_dir: Path, result) -> dict:
    """Copy what the run left in the data root; return the timing and verdict."""
    case_dir.mkdir(parents=True, exist_ok=True)
    for name in ("dump.csv", "trace.csv", "grouptrace_last.txt"):
        src = worker.data / name
        if src.exists():
            shutil.copyfile(src, case_dir / name)
    (case_dir / "result_lines.txt").write_text(
        "\n".join(verdict_lines(result.lines)) + "\n", encoding="utf-8")
    return {"outcome": result.outcome, "tick": result.tick, "x": round(result.x, 3),
            "pct": round(result.pct, 4)}


def run_arm_fresh(cases, worker_id, out: Path, cfg: dict, mod: Path) -> list[dict]:
    meta = []
    w = W.Worker(worker_id)
    try:
        for i, c in enumerate(cases):
            t0 = time.time()
            w.open(c["level"], dict(cfg, serverearm="0"), mod)
            t1 = time.time()
            r = w.run(c["plan"], timeout_s=300)
            t2 = time.time()
            d = out / "F" / f"{i:02d}_lv{c['level']}_{c['name']}"
            m = snap(w, d, r)
            m.update(open_s=round(t1 - t0, 2), run_s=round(t2 - t1, 2))
            meta.append(m)
            print(f"  F {i:02d} lv{c['level']} {c['name']:6s} open {t1 - t0:5.1f}s "
                  f"run {t2 - t1:5.1f}s  {m['outcome']} t={m['tick']} x={m['x']}")
            w.close()
    finally:
        w.close()
    return meta


def run_arm_resident(cases, worker_id, out: Path, cfg: dict, mod: Path,
                     arm: str = "R") -> list[dict]:
    """arm R: the next plan on the level in hand is a reset in place. arm S: every case first
    enters its level afresh (a switch, also to the level it is already on)."""
    meta = []
    w = W.Worker(worker_id)
    try:
        for i, c in enumerate(cases):
            t0 = time.time()
            if arm == "R" and w.is_alive() and w.level == c["level"] and w.rearm:
                sw = None          # same level: the next plan goes in as a reset in place
            else:
                sw = w.switch(c["level"], dict(cfg, serverearm="1"), mod)
            t1 = time.time()
            r = w.run(c["plan"], timeout_s=300, relaunch=False)
            t2 = time.time()
            d = out / arm / f"{i:02d}_lv{c['level']}_{c['name']}"
            m = snap(w, d, r)
            m.update(switch_s=round(t1 - t0, 2), run_s=round(t2 - t1, 2),
                     switched=None if sw is None else sw.get("switched"))
            meta.append(m)
            how = "reset" if sw is None else ("switch" if sw.get("switched") else "RELAUNCH")
            print(f"  {arm} {i:02d} lv{c['level']} {c['name']:6s} {how:8s} {t1 - t0:5.1f}s "
                  f"run {t2 - t1:5.1f}s  {m['outcome']} t={m['tick']} x={m['x']}")
    finally:
        w.close()
    return meta


def run_arm_tools(cases, worker_id, out: Path, cfg: dict, mod: Path) -> list[dict]:
    """arm T: the MCP tools themselves, as a client calls them -- gd_session_open for each case's
    level, then gd_run with its defaults. This is the arm whose equality is the claim the tools
    make; S is the same thing written out by hand."""
    meta = []
    prev = None
    try:
        for i, c in enumerate(cases):
            t0 = time.time()
            # a client opens a session once per level and then keeps running plans on it, so the
            # plans after the first on a level reach gd_run with the level already used (dirty)
            info = {}
            if c["level"] != prev:
                info = T.gd_session_open(c["level"], worker_id, dict(cfg), str(mod))
                prev = c["level"]
            t1 = time.time()
            s = T.gd_run(plan=[list(p) for p in c["plan"]], timeout_s=300)
            t2 = time.time()
            r = T._last["result"]
            d = out / "T" / f"{i:02d}_lv{c['level']}_{c['name']}"
            m = snap(T._session, d, r)
            m.update(open_s=round(t1 - t0, 2), run_s=round(t2 - t1, 2),
                     switched=info.get("switched"), entered=s.get("entered"))
            meta.append(m)
            print(f"  T {i:02d} lv{c['level']} {c['name']:6s} open {t1 - t0:5.1f}s "
                  f"({'switch' if info.get('switched') else 'launch'}) run {t2 - t1:5.1f}s "
                  f"[{s.get('entered')}]  {m['outcome']} t={m['tick']} x={m['x']}")
    finally:
        T.gd_session_close()
    return meta


def read_rows(p: Path) -> tuple[list[str], list[list[str]]]:
    if not p.exists():
        return [], []
    with p.open(newline="", encoding="utf-8-sig", errors="replace") as f:
        rows = list(csv.reader(f))
    return (rows[0], rows[1:]) if rows else ([], [])


# trace.csv events that mark where the game loop cut a frame, not what the game did. Where the cut
# falls depends on the state the loop was in when the plan arrived (a worker that stopped at a clear
# is still "locked" for the first frame and runs one tick in it), so the same ticks are batched
# differently. Everything between the cuts is compared.
FRAME_EVENTS = {"BGL_update_pre", "BGL_update_post"}


def trace_rows(rows: list[list[str]], head: list[str]) -> list[list[str]]:
    """From the attempt's own reset on, without the frame-cut events (see FRAME_EVENTS). The rows
    before it belong to the attempt that was stopped when the plan was supplied."""
    ev = head.index("event")
    start = next((i for i, r in enumerate(rows) if len(r) > ev and r[ev] == "resetLevel"), 0)
    return [r for r in rows[start:] if len(r) > ev and r[ev] not in FRAME_EVENTS]


# The dump columns known to hold a value GD's own reset leaves as the last attempt had it and
# that is read only after something has written it (the flight band's height, the stair-snap
# distance, two ground flags of the first row). A column NOT in this list is never excused, however
# it differs.
STALE_OK = {"pmin", "pmax", "snapdist", "onGround2", "p2ground2"}


def stale_prefix(ra, rb, col: int, rows: list[int], name: str) -> dict | None:
    """A column that differs ONLY from the first row until some row, with one constant value on
    each side over that stretch, is a field the level had not written yet: both sides then agree
    from its first live write on. (GD's own reset leaves such fields as the last attempt had them,
    so a plain retry in a real game shows the same stale values.) Returns the description, or None
    if the differences are anything else."""
    if name not in STALE_OK:
        return None
    if not rows or rows[0] != 0 or rows != list(range(len(rows))):
        return None
    fv = {ra[i][col] for i in rows}
    rv = {rb[i][col] for i in rows}
    if len(fv) != 1 or len(rv) != 1:
        return None
    return {"rows": len(rows), "F": next(iter(fv)), "R": next(iter(rv))}


def compare_csv(a: Path, b: Path, kind: str = "dump") -> dict:
    ha, ra = read_rows(a)
    hb, rb = read_rows(b)
    if kind == "trace" and ha and hb:
        ra, rb = trace_rows(ra, ha), trace_rows(rb, hb)
    out = {"rows_F": len(ra), "rows_R": len(rb), "diff_rows": 0, "stale": {}}
    if ha != hb:
        out["header_differs"] = [ha, hb]
    keep = [i for i, h in enumerate(ha) if h not in IGNORED_COLS]
    per_col: dict[int, list[int]] = {}
    for n, (x, y) in enumerate(zip(ra, rb)):
        for i in keep:
            if (x[i] if i < len(x) else None) != (y[i] if i < len(y) else None):
                per_col.setdefault(i, []).append(n)
    hard_rows: set[int] = set()
    colhits: dict[str, int] = {}
    for i, rows in per_col.items():
        if kind == "dump":
            st = stale_prefix(ra, rb, i, rows, ha[i])
            if st:
                out["stale"][ha[i]] = st
                continue
        colhits[ha[i]] = len(rows)
        hard_rows.update(rows)
    out["diff_rows"] = len(hard_rows) + abs(len(ra) - len(rb))
    if hard_rows:
        n = min(hard_rows)
        x, y = ra[n], rb[n]
        out["first"] = {"row": n, "tick": x[ha.index("tick")] if "tick" in ha else None,
                        "cols": [dict(col=ha[i], F=x[i], R=y[i]) for i in keep
                                 if ha[i] in colhits and i < len(x) and x[i] != y[i]][:6]}
        out["by_col"] = dict(sorted(colhits.items(), key=lambda kv: -kv[1])[:12])
    return out


def compare_text(a: Path, b: Path, drop: tuple = ()) -> dict:
    def read(p: Path):
        if not p.exists():
            return None
        t = p.read_text(encoding="utf-8", errors="replace")
        if drop:
            t = "\n".join(l for l in t.splitlines() if not l.startswith(drop)) + "\n"
        return t
    ta, tb = read(a), read(b)
    if ta == tb:
        return {"identical": True, "bytes": len(ta or "")}
    la, lb = (ta or "").splitlines(), (tb or "").splitlines()
    for n in range(max(len(la), len(lb))):
        x = la[n] if n < len(la) else None
        y = lb[n] if n < len(lb) else None
        if x != y:
            return {"identical": False, "line": n, "F": (x or "")[:200], "R": (y or "")[:200],
                    "lines_F": len(la), "lines_R": len(lb)}
    return {"identical": False}


def compare(cases, out: Path, arm: str = "R") -> tuple[list[dict], int]:
    report, bad = [], 0
    for i, c in enumerate(cases):
        tag = f"{i:02d}_lv{c['level']}_{c['name']}"
        f, r = out / "F" / tag, out / arm / tag
        item = {"case": tag}
        for name in ("dump.csv", "trace.csv"):
            item[name] = compare_csv(f / name, r / name,
                                     "trace" if name == "trace.csv" else "dump")
        item["grouptrace_last.txt"] = compare_text(f / "grouptrace_last.txt",
                                                    r / "grouptrace_last.txt")
        item["result_lines"] = compare_text(f / "result_lines.txt", r / "result_lines.txt",
                                            drop=("clearcheck:", "endtrigger:"))
        ok = (item["dump.csv"]["diff_rows"] == 0 and item["trace.csv"]["diff_rows"] == 0
              and "header_differs" not in item["dump.csv"]
              and "header_differs" not in item["trace.csv"]
              and item["grouptrace_last.txt"].get("identical")
              and item["result_lines"].get("identical")
              and item["dump.csv"]["rows_F"] > 0)
        item["equal"] = bool(ok)
        bad += 0 if ok else 1
        report.append(item)
    return report, bad


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(prog="gdmcp.equiv", description=__doc__.split("\n")[0])
    ap.add_argument("--worker", type=int, required=True)
    ap.add_argument("--levels", type=int, nargs="+", required=True)
    ap.add_argument("--plan-dir", default=str(REPO / "data"))
    ap.add_argument("--death-inputs", type=int, default=3,
                    help="inputs of the stored plan kept for the run that is meant to die")
    ap.add_argument("--out", required=True)
    ap.add_argument("--cfg", action="append", default=[], metavar="K=V",
                    help="extra autorun.cfg keys for BOTH arms (added to the rich observation set)")
    ap.add_argument("--mod", default=str(W.BUILD_MOD),
                    help="the .geode to measure (both arms; its sha1 is recorded)")
    ap.add_argument("--leveldir", default=None,
                    help="a directory of <id>.lvl for levels that are not main levels "
                         "(cfg leveldir; the plans are still read from --plan-dir)")
    ap.add_argument("--arms", nargs="*", default=["F", "R", "S", "T"], choices=["F", "R", "S", "T"],
                    help="which arms to RUN (the others are read from --out); compare is always "
                         "F against R and S, whatever exists")
    a = ap.parse_args(argv)
    out = Path(a.out)
    out.mkdir(parents=True, exist_ok=True)
    cfg = dict(RICH_CFG)
    if a.leveldir:
        cfg["leveldir"] = a.leveldir.replace("\\", "/")
    for kv in a.cfg:
        k, _, v = kv.partition("=")
        cfg[k] = v
    cases = make_cases(a.levels, Path(a.plan_dir), a.death_inputs)
    meta: dict = {"mod": a.mod, "cfg": cfg, "cases": [(c["level"], c["name"], len(c["plan"])) for c in cases]}
    if "F" in a.arms:
        print("== arm F: a fresh game per case")
        meta["F"] = run_arm_fresh(cases, a.worker, out, cfg, Path(a.mod))
    if "R" in a.arms:
        print("== arm R: one resident game, reset in place between plans")
        meta["R"] = run_arm_resident(cases, a.worker, out, cfg, Path(a.mod), "R")
    if "S" in a.arms:
        print("== arm S: one resident game, the level entered afresh before every plan")
        meta["S"] = run_arm_resident(cases, a.worker, out, cfg, Path(a.mod), "S")
    if "T" in a.arms:
        print("== arm T: the MCP tools, defaults")
        meta["T"] = run_arm_tools(cases, a.worker, out, cfg, Path(a.mod))
    bad_total = 0
    for arm in ("R", "S", "T"):
        if not (out / arm).exists():
            continue
        report, bad = compare(cases, out, arm)
        bad_total += bad
        meta["report_" + arm] = report
        print(f"== compare F against {arm} (every row; columns frame/attempt excluded)")
        for it in report:
            d, t = it["dump.csv"], it["trace.csv"]
            print(f"  {'EQUAL ' if it['equal'] else 'DIFFER'} {it['case']:24s} "
                  f"dump {d['rows_F']}/{d['rows_R']} rows diff={d['diff_rows']}  "
                  f"trace {t['rows_F']}/{t['rows_R']} diff={t['diff_rows']}  "
                  f"gt={'=' if it['grouptrace_last.txt'].get('identical') else 'DIFF'} "
                  f"res={'=' if it['result_lines'].get('identical') else 'DIFF'}")
            if it["dump.csv"]["stale"]:
                print("      stale-until-first-write (not counted):",
                      {c: f"{s['rows']} rows F={s['F']} R={s['R']}"
                       for c, s in it["dump.csv"]["stale"].items()})
            if not it["equal"]:
                for k in ("dump.csv", "trace.csv"):
                    if it[k].get("first"):
                        print(f"      {k} first: {it[k]['first']}  by_col={it[k].get('by_col')}")
                for k in ("grouptrace_last.txt", "result_lines"):
                    if not it[k].get("identical"):
                        print(f"      {k}: {it[k]}")
        print(f"{arm}: {len(report) - bad}/{len(report)} cases equal")
    # keep what earlier invocations recorded about arms this one did not run
    prev = out / "equiv.json"
    if prev.exists():
        try:
            old = json.loads(prev.read_text(encoding="utf-8"))
            meta = {**old, **meta}
        except ValueError:
            pass
    prev.write_text(json.dumps(meta, indent=1, default=str), encoding="utf-8")
    return 0 if bad_total == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
