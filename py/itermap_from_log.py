"""Build an iteration map from a run's log, for the in-game F10 overlay.

The repair loop writes `data/itermap_lv<N>.txt` itself when it finishes (see
src/mod/itermap.hpp).  Every run made before that existed only left its log --
and the logs already hold everything the map needs, so the maps of the runs on
disk can be recovered instead of re-solved:

    python py/itermap_from_log.py --all           # every data/coldlog_lv*.txt
    python py/itermap_from_log.py data/coldlog_lv22.txt
    python py/itermap_from_log.py data/solvelog_live_lv16.txt --print

What is read, and from which line:

    dpsolve: iter N: death t=T x=X            one round, and where GD stopped it
    killer: t=T ... py=Y ... uid=U id=I       GD's own verdict on that death
    dpsolve:   no improvement ...             the round was scored as a rewind
    dpsolve:   regression ... solved branch   ...as a followed solved branch
    dpsolve:   ... on the forced route        ...as the forced portal route
    dpsolve:   wedged since / void attempt    ...as a wedge (never ranks)
    dpsolve:   [fixup] t=T x=X ... kill=K     where the MODEL was wrong
    dpsolve:   [veto] ... --deadband x0,x1,.. a stretch ruled out as a route
    dpsolve:   re-anchored at t=T (backoff B) what the NEXT round was built from
    dpsolve:   [anchor] --start T,X,...       ...and where that anchor sat

ONE APPROXIMATION, and it is in the file as `approx=fixup_y`: the `[fixup]`
line carries x but no y, so a fixup is drawn at the y of its own round's death.
Its x -- the part that says where the model was wrong -- is exact.  Maps the
mod writes itself carry the real y and no `approx=` line.
"""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

from gdtas.paths import DATA  # noqa: E402

# Kinds, in the order src/mod/itermap.hpp defines them.
DEEPER, FOLLOW, FORCED, REWIND, WEDGE = range(5)
KIND_NAME = {DEEPER: "deeper", FOLLOW: "followed", FORCED: "forced",
             REWIND: "rewound", WEDGE: "wedged"}

RE_ITER = re.compile(r"^dpsolve: iter (\d+): death t=(\d+) x=(-?[\d.]+)")
RE_KILLER = re.compile(
    r"^killer: t=(\d+) who=(\S+) py=(-?[\d.]+) pvy=\S+ px=(-?[\d.]+) "
    r"obj=\S+ uid=(-?\d+) id=(-?\d+)")
RE_FIXUP = re.compile(
    r"^dpsolve:\s+\[fixup\] t=(\d+) x=(-?[\d.]+) mode=\d+ in=\d+ "
    r"dy=\S+ dvy=\S+ kill=(\d)")
RE_VETO = re.compile(r"^dpsolve:\s+\[veto\] .*--deadband (-?[\d.]+),(-?[\d.]+),")
RE_REANCHOR = re.compile(r"^dpsolve:\s+re-anchored at t=(\d+) \(backoff (\d+)\)")
RE_ANCHOR = re.compile(r"^dpsolve:\s+\[anchor\] --start (\d+),(-?[\d.]+),")
RE_LEVEL = re.compile(r"^levelinfo: id=(\d+)")
# Two ways a log says the run cleared, and a headless one only has the second.
# `cleared after N repair rounds` is printed by onCleared(), which runs only when
# the solution is going to be SHOWN -- a worker has no screen to show it on.  The
# saved solution is the other half of the same moment: the mod files a plan under
# that name only once GD has been seen to clear the level on it.
RE_CLEARED = re.compile(r"^dpsolve: (cleared after \d+ repair rounds|solution saved ->)")
RE_WEDGE = re.compile(r"^dpsolve:\s+(wedged since|void attempt)")
RE_REWIND = re.compile(r"^dpsolve:\s+no improvement")
RE_FOLLOW = re.compile(r"^dpsolve:\s+regression to t=\d+ on a solved branch")
RE_FORCED = re.compile(r"^dpsolve:\s+(progress to|regression to) t=\d+ on the forced route")


class Run:
    def __init__(self) -> None:
        self.level: int | None = None
        self.cleared = False
        self.deaths: list[dict] = []
        self.fixups: list[dict] = []
        self.vetoes: list[tuple[int, float, float]] = []
        self.rounds = 0


def parse(path: Path) -> Run:
    """One pass over the log.

    The order the loop prints in is what makes this a single pass: the credit
    lines (wedge, off-board) come BEFORE the `iter N:` line, the branch that
    scored the round comes AFTER it, and the re-anchor that produced the plan
    for the NEXT round comes after that.  So a round is closed when the round
    following it opens, not when its own line is read.
    """
    run = Run()
    killers: dict[int, tuple[float, int, int]] = {}   # tick -> (py, id, uid)
    anchor_x: dict[int, float] = {}                   # anchor tick -> x
    pending_wedge = False
    pending: dict | None = None          # the round whose kind is still open
    next_anchor: tuple[int, int] | None = None   # (tick, backoff) for the next round
    fixups_open: list[dict] = []         # fixups seen since the last round opened

    with path.open("r", encoding="utf-8", errors="replace") as fh:
        for line in fh:
            line = line.rstrip("\n")

            m = RE_KILLER.match(line)
            if m:
                if m.group(2) == "p1":
                    killers[int(m.group(1))] = (
                        float(m.group(3)), int(m.group(6)), int(m.group(5)))
                continue

            m = RE_ANCHOR.match(line)
            if m:
                anchor_x[int(m.group(1))] = float(m.group(2))
                continue

            m = RE_REANCHOR.match(line)
            if m:
                next_anchor = (int(m.group(1)), int(m.group(2)))
                continue

            m = RE_FIXUP.match(line)
            if m:
                fixups_open.append({
                    "iter": run.rounds,
                    "tick": int(m.group(1)),
                    "x": float(m.group(2)),
                    "kill": int(m.group(3)),
                })
                continue

            m = RE_VETO.match(line)
            if m:
                run.vetoes.append((run.rounds, float(m.group(1)), float(m.group(2))))
                continue

            if RE_WEDGE.match(line):
                pending_wedge = True
                continue

            if pending is not None:
                # The scoring lines, only meaningful while a round is open.
                if RE_REWIND.match(line):
                    pending["kind"] = REWIND
                    continue
                if RE_FOLLOW.match(line):
                    pending["kind"] = FOLLOW
                    continue
                if RE_FORCED.match(line):
                    pending["kind"] = FORCED
                    continue

            m = RE_ITER.match(line)
            if m:
                # Close the previous round: its kind and its fixups are known now.
                if pending is not None:
                    run.deaths.append(pending)
                    for f in fixups_open:
                        f["y"] = pending["y"]
                        run.fixups.append(f)
                fixups_open = []
                it, tick, x = int(m.group(1)), int(m.group(2)), float(m.group(3))
                py, kid, kuid = killers.get(tick, (0.0, -1, -1))
                at, backoff = next_anchor if next_anchor else (-1, 0)
                ax = anchor_x.get(at, 0.0) if at >= 0 else 0.0
                pending = {
                    "iter": it, "tick": tick, "x": x, "y": py,
                    # Deeper is the branch that prints nothing of its own, so it
                    # is what a round with none of the other three lines was.
                    "kind": WEDGE if pending_wedge else DEEPER,
                    "anchorT": at, "anchorX": ax, "backoff": backoff,
                    "killerId": kid, "killerUid": kuid,
                }
                pending_wedge = False
                next_anchor = None
                run.rounds = max(run.rounds, it)
                continue

            m = RE_LEVEL.match(line)
            if m:
                run.level = int(m.group(1))
                continue

            if RE_CLEARED.match(line):
                run.cleared = True

    if pending is not None:
        run.deaths.append(pending)
        for f in fixups_open:
            f["y"] = pending["y"]
            run.fixups.append(f)
    # A wedge that the loop scored as one still fell down a branch that printed
    # its own line; the wedge wins, exactly as it does in onDeath.
    return run


def write_map(run: Run, out: Path) -> None:
    lines = ["# gdsolver iteration map (rebuilt from a log by py/itermap_from_log.py)",
             "approx=fixup_y",
             f"level={run.level if run.level is not None else -1}",
             f"rounds={run.rounds}",
             f"cleared={1 if run.cleared else 0}"]
    for d in run.deaths:
        lines.append("death={iter},{tick},{x:g},{y:g},{kind},{anchorT},{anchorX:g},"
                     "{backoff},{killerId},{killerUid}".format(**d))
    for f in run.fixups:
        lines.append("fixup={iter},{tick},{x:g},{y:g},{kill}".format(**f))
    for it, x0, x1 in run.vetoes:
        lines.append(f"veto={it},{x0:g},{x1:g}")
    out.write_text("\n".join(lines) + "\n", encoding="utf-8")


def report(run: Run, out: Path) -> str:
    """The same picture the strip draws, in one paragraph of text."""
    bucket = 30.0
    by: dict[int, list[int]] = {}
    for d in run.deaths:
        by.setdefault(int(d["x"] // bucket), []).append(d["kind"])
    hot = sorted(by.items(), key=lambda kv: -len(kv[1]))[:5]
    total = max(1, len(run.deaths))
    parts = []
    for b, kinds in hot:
        dom = max(set(kinds), key=kinds.count)
        parts.append(f"x={int(b * bucket + bucket / 2)} ({len(kinds)}, {KIND_NAME[dom]})")
    return (f"lv{run.level}: {run.rounds} rounds, {len(run.deaths)} deaths, "
            f"{len(run.fixups)} fixups, {len(run.vetoes)} vetoes"
            f"{'' if run.cleared else ', DID NOT CLEAR'}\n"
            f"  hottest {round(100 * len(hot[0][1]) / total) if hot else 0}% of deaths at "
            + ", ".join(parts) + f"\n  -> {out}")


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("logs", nargs="*", type=Path,
                    help="log files (data/coldlog_lvN.txt, data/solvelog_live_lvN.txt)")
    ap.add_argument("--all", action="store_true",
                    help="every data/coldlog_lv*.txt")
    ap.add_argument("--out-dir", type=Path, default=DATA,
                    help="where the maps go (default: data/)")
    ap.add_argument("--print", dest="show", action="store_true",
                    help="also print the hotspots")
    args = ap.parse_args()

    logs = list(args.logs)
    if args.all:
        logs += sorted(DATA.glob("coldlog_lv*.txt"))
    if not logs:
        ap.error("nothing to do: name a log, or pass --all")

    made = 0
    for log in logs:
        if not log.exists():
            print(f"{log}: no such file", file=sys.stderr)
            continue
        run = parse(log)
        if run.level is None:
            m = re.search(r"lv(\d+)", log.stem)
            run.level = int(m.group(1)) if m else None
        if run.level is None:
            print(f"{log}: no level id in the log or the name - skipped", file=sys.stderr)
            continue
        if not run.deaths:
            # Not a failure: five of the official levels are solved by the first
            # plan the model produces, and a run with no rounds has no map.
            print(f"lv{run.level}: no repair rounds in this log - nothing to map")
            continue
        args.out_dir.mkdir(parents=True, exist_ok=True)
        out = args.out_dir / f"itermap_lv{run.level}.txt"
        write_map(run, out)
        made += 1
        print(report(run, out) if args.show
              else f"lv{run.level}: {run.rounds} rounds -> {out}")
    return 0 if made else 1


if __name__ == "__main__":
    raise SystemExit(main())
