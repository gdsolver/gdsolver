"""Sweeps over plans on one resident game, as a table.

The MCP tools answer one question per call; the questions that keep coming back are the same
call many times with one thing moved. This does that from a command line, through the same
functions the MCP server calls (gdmcp.tools), so a sweep is a client of exactly what an
analysis session is a client of: one GD held open, the level entered afresh before every run
(equal to a relaunch, see equiv.py) unless --fast.

    # shift one press: the 12th input of the plan moved by -3..+3 ticks
    python -m gdmcp.sweep --level 16 --plan data/solution_lv16_dp.txt press --index 11 --by=-3:3

    # every input from tick 4000 on, moved together
    python -m gdmcp.sweep --level 16 --plan P press --from-tick 4000 --by=-2:2

    # inject y / vy on a grid at tick 380 and look 30 ticks on
    python -m gdmcp.sweep --level 1 --plan P inject --tick 380 --y 100:112:7 --vy 0:12:5 --after 30

    # a list of plans, one row each
    python -m gdmcp.sweep --level 16 plans a.txt b.txt c.txt

Ranges are `start:stop:count` (count points, both ends in; `--by` takes integers `a:b` with step 1
or `a:b:step`). Every row says how it ended -- death with its tick and x, or complete, or for
`inject` the state `--after` ticks later -- and the table is also written as CSV with --csv.
The worker is one of 99 and up; take it from the lease, not at random (the repository's
CLAUDE.md and the worker notes say who is using which).
"""

from __future__ import annotations

import argparse
import csv
import sys
import time
from pathlib import Path

from . import tools as T


def frange(spec: str) -> list[float]:
    a, b, n = spec.split(":")
    a, b, n = float(a), float(b), int(n)
    return [a] if n <= 1 else [a + (b - a) * i / (n - 1) for i in range(n)]


def irange(spec: str) -> list[int]:
    p = [int(x) for x in spec.split(":")]
    lo, hi, step = p[0], p[1], (p[2] if len(p) > 2 else 1)
    return list(range(lo, hi + 1, step))


def shift_plan(plan: list[tuple[int, int]], by: int, index: int | None,
               from_tick: int | None) -> list[tuple[int, int]]:
    out = []
    for i, (t, d) in enumerate(plan):
        hit = (index is not None and i == index) or (from_tick is not None and t >= from_tick)
        out.append((max(0, t + by) if hit else t, d))
    return sorted(out)


def describe(s: dict) -> dict:
    """The columns of one row, from gd_run's summary."""
    st = s.get("state") or {}
    return {"outcome": s["outcome"], "tick": s["tick"], "x": s["x"],
            "y": st.get("y", ""), "vy": st.get("yvel", ""), "mode": st.get("mode", ""),
            "pct": s.get("pct", "")}


def table(rows: list[dict], keys: list[str]) -> str:
    w = {k: max(len(k), *(len(str(r.get(k, ""))) for r in rows)) for k in keys}
    head = "  ".join(k.ljust(w[k]) for k in keys)
    return "\n".join([head, "  ".join("-" * w[k] for k in keys)] +
                     ["  ".join(str(r.get(k, "")).ljust(w[k]) for k in keys) for r in rows])


def run_call(a) -> int:
    """`call` sweep: the cartesian product of the grid axes, one gd_call per point."""
    import itertools
    import json
    spec = json.loads(a.spec.read_text(encoding="utf-8-sig"))
    axes = []
    for g in spec.get("grid", []):
        vals = g.get("values")
        if vals is None:
            vals = frange(g["range"])
        axes.append((g, vals))
    cfg = dict(kv.split("=", 1) for kv in a.cfg)
    cfg.setdefault("callverify", "1")
    t0 = time.time()
    print(f"# opening lv{a.level} on worker {a.worker}", flush=True)
    T.gd_session_open(a.level, a.worker, cfg, a.mod)
    # serve commands are only read while a level is held: run a few ticks and stop there
    T.gd_run(plan=[[0, 0]], stop_at=60, timeout_s=a.timeout)
    rows = []
    n = 1
    for _, v in axes:
        n *= len(v)
    print(f"# {n} calls of {spec['function']}", flush=True)
    try:
        for point in itertools.product(*[v for _, v in axes]):
            patches = list(spec.get("base", []))
            row = {}
            for (g, _), val in zip(axes, point):
                patches.append({"who": g["who"], "offset": g["offset"], "type": g["type"], "value": val})
                row[g.get("name", f"{g['who']}+{g['offset']:#x}")] = round(val, 6) if isinstance(val, float) else val
            r = T.gd_call(spec["function"], patches, spec.get("args"), spec.get("read"), timeout_s=a.timeout)
            if not r.get("ok"):
                row["error"] = r.get("error", "?")
            else:
                row.update(r.get("read", {}))
            rows.append(row)
    finally:
        T.gd_session_close()
    cols = []
    for r in rows:
        for k in r:
            if k not in cols:
                cols.append(k)
    print(table(rows, cols))
    if a.csv:
        with a.csv.open("w", newline="", encoding="utf-8") as f:
            w = csv.DictWriter(f, fieldnames=cols, extrasaction="ignore")
            w.writeheader()
            w.writerows(rows)
        print(f"# wrote {a.csv}")
    print(f"# {len(rows)} calls in {time.time() - t0:.0f}s")
    return 0


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(prog="gdmcp.sweep", description=__doc__.split("\n")[0])
    ap.add_argument("--level", type=int, required=True)
    ap.add_argument("--worker", type=int, default=100)
    ap.add_argument("--plan", help="the base plan (input=t,d lines)")
    ap.add_argument("--cfg", action="append", default=[], metavar="K=V")
    ap.add_argument("--mod", default=None, help="the .geode (default: the repository's build)")
    ap.add_argument("--fast", action="store_true",
                    help="reset in place between runs (about 0.5 s instead of 2 s; not bit-equal "
                         "to a relaunch -- see mcp/README.md)")
    ap.add_argument("--timeout", type=float, default=300.0)
    ap.add_argument("--csv", type=Path, default=None)
    sub = ap.add_subparsers(dest="kind", required=True)

    p = sub.add_parser("press", help="move one press, or every press from a tick on")
    p.add_argument("--index", type=int, default=None)
    p.add_argument("--from-tick", type=int, default=None)
    p.add_argument("--by", default="-3:3", help="shift range in ticks, a:b or a:b:step; write it --by=-3:3 (a leading minus is read as an option)")

    i = sub.add_parser("inject", help="inject x / y / vy on a grid at one tick")
    i.add_argument("--tick", type=int, required=True)
    i.add_argument("--x", default=None, help="start:stop:count")
    i.add_argument("--y", default=None, help="start:stop:count")
    i.add_argument("--vy", default=None, help="start:stop:count")
    i.add_argument("--after", type=int, default=30, help="ticks to look on after the injection")

    q = sub.add_parser("plans", help="a list of plan files, one row each")
    q.add_argument("files", nargs="+")

    c = sub.add_parser("call", help="GD's own function on a scratch object, over a grid of inputs (needs cfg callverify=1; see mcp/README.md, gd_call)")
    c.add_argument("--spec", type=Path, required=True,
                   help="a JSON file: {function, args, base:[patch...], grid:[{who,offset,type,range|values}...], read:[{name,who,offset,type}...]}")

    a = ap.parse_args(argv)
    if a.kind == "call":
        return run_call(a)
    cfg = dict(kv.split("=", 1) for kv in a.cfg)
    base = T.parse_plan_file(a.plan) if a.plan else []

    jobs: list[tuple[dict, list, dict]] = []     # (row labels, plan, gd_run kwargs)
    if a.kind == "press":
        if (a.index is None) == (a.from_tick is None):
            ap.error("press: give exactly one of --index / --from-tick")
        for by in irange(a.by):
            jobs.append(({"by": by}, shift_plan(base, by, a.index, a.from_tick), {}))
        keys = ["by"]
    elif a.kind == "inject":
        xs = frange(a.x) if a.x else [None]
        ys = frange(a.y) if a.y else [None]
        vs = frange(a.vy) if a.vy else [None]
        for x in xs:
            for y in ys:
                for v in vs:
                    spec = {"tick": a.tick}
                    for k, val in (("x", x), ("y", y), ("vy", v)):
                        if val is not None:
                            spec[k] = round(val, 6)
                    jobs.append(({"inj_x": "" if x is None else round(x, 3),
                                  "inj_y": "" if y is None else round(y, 3),
                                  "inj_vy": "" if v is None else round(v, 3)}, base,
                                 {"inject": [spec], "stop_at": a.tick + a.after}))
        keys = ["inj_x", "inj_y", "inj_vy"]
    else:
        for f in a.files:
            jobs.append(({"plan": Path(f).name}, T.parse_plan_file(f), {}))
        keys = ["plan"]

    t0 = time.time()
    print(f"# opening lv{a.level} on worker {a.worker}", flush=True)
    info = T.gd_session_open(a.level, a.worker, cfg, a.mod)
    print(f"# {'switched' if info.get('switched') else 'launched'} in {time.time() - t0:.1f}s; "
          f"{len(jobs)} runs", flush=True)
    rows = []
    try:
        for lab, plan, kw in jobs:
            t1 = time.time()
            s = T.gd_run(plan=[list(x) for x in plan] or [[0, 0]], timeout_s=a.timeout,
                         in_place=a.fast, **kw)
            row = {**lab, **describe(s), "s": round(time.time() - t1, 1)}
            if s.get("warning"):
                row["note"] = "injection dropped"
            rows.append(row)
    finally:
        T.gd_session_close()
    cols = keys + ["outcome", "tick", "x", "y", "vy", "mode", "s"]
    if any("note" in r for r in rows):
        cols.append("note")
    print(table(rows, cols))
    if a.csv:
        with a.csv.open("w", newline="", encoding="utf-8") as f:
            w = csv.DictWriter(f, fieldnames=cols, extrasaction="ignore")
            w.writeheader()
            w.writerows(rows)
        print(f"# wrote {a.csv}")
    print(f"# {len(rows)} runs in {time.time() - t0:.0f}s")
    return 0


if __name__ == "__main__":
    sys.exit(main())
