"""A CLI that calls the same tools without going through MCP.

There are two uses:
  1. Sanity checking. The MCP server needs the client (Claude Code) to be restarted,
     so before wiring it up you can confirm here that GD really runs and returns.
  2. Fault isolation. When nothing comes back, this separates the MCP layer from GD.

A session only lives as long as one process, so the CLI pays the 20 second launch
every time. If you want the benefit of a resident session, use the MCP side.

    python -m gdmcp.cli smoke --level 1 --plan-file data/solution_lv1_dp.txt
    python -m gdmcp.cli objects --level 1 --x0 500 --x1 800
"""

from __future__ import annotations

import argparse
import json
import sys

from . import data as D
from . import tools
from .worker import Worker


def _p(obj) -> None:
    print(json.dumps(obj, ensure_ascii=False, indent=2, default=str))


def cmd_smoke(a) -> int:
    info = tools.gd_session_open(a.level, a.worker, {})
    _p(info)
    try:
        r = tools.gd_run(plan_file=a.plan_file, timeout_s=a.timeout)
        _p(r)
        if r["outcome"] == "death":
            _p(tools.gd_death_context(back_ticks=a.back))
        else:
            _p(tools.gd_trace(t0=max(0, r["tick"] - 20), stride=4, limit=10))
        return 0 if r["outcome"] in ("death", "complete") else 1
    finally:
        tools.gd_session_close()


def cmd_objects(a) -> int:
    """No session needed: the table is invariant per level, so read it from the file."""
    w = Worker(a.worker)
    path = D.objects_path(w.data, a.level)
    _p(D.read_objects(path, a.x0, a.x1, limit=a.limit))
    return 0


def cmd_diff(a) -> int:
    """No session needed: it only matches an existing dump against the model trace.

    You do not have to reopen MCP for every run - it works directly on a local dump
    such as `solution_lvN_dp.txt.best.dump`.
    """
    from pathlib import Path
    _p(D.diff_trace(Path(a.trace), Path(a.dump), a.t0, a.t1, a.tol, a.limit))
    return 0


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(prog="gdmcp.cli")
    sub = ap.add_subparsers(dest="cmd", required=True)

    s = sub.add_parser("smoke", help="launch, run one plan, read the result, "
                                     "all in one process")
    s.add_argument("--level", type=int, default=1)
    s.add_argument("--worker", type=int, default=98)
    s.add_argument("--plan-file", required=True)
    s.add_argument("--timeout", type=float, default=180.0)
    s.add_argument("--back", type=int, default=24)
    s.set_defaults(fn=cmd_smoke)

    o = sub.add_parser("objects", help="look up the object table with hit rects")
    o.add_argument("--level", type=int, default=1)
    o.add_argument("--worker", type=int, default=98)
    o.add_argument("--x0", type=float, required=True)
    o.add_argument("--x1", type=float, required=True)
    o.add_argument("--limit", type=int, default=100)
    o.set_defaults(fn=cmd_objects)

    d = sub.add_parser("diff", help="print the first divergence between the "
                                    "model trace and the GD dump")
    d.add_argument("--trace", required=True, help="leveldp's <plan>.trace.csv")
    d.add_argument("--dump", required=True, help="GD's dump.csv / *.best.dump")
    d.add_argument("--t0", type=int, default=0)
    d.add_argument("--t1", type=int, default=None)
    d.add_argument("--tol", type=float, default=0.3)
    d.add_argument("--limit", type=int, default=8)
    d.set_defaults(fn=cmd_diff)

    a = ap.parse_args(argv)
    return a.fn(a)


if __name__ == "__main__":
    sys.exit(main())
