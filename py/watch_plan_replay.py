"""Replay a solved (or partial) plan at 1x in a window, for a human to watch.

    python py/watch_plan_replay.py --level 16 --worker-id 91

The solver's own loop runs headless and in batches (fastloops), so there is
nothing to see. Here the plan is handed to autorun.cfg as `input=` lines and run
with solve=0, which makes it a plain replay at 1x with the artwork and the music.
The default plan is data/solution_lv{N}_dp.txt.best.

  * By default every other worker is killed: the one being watched needs the
    foreground and its own resolution. Pass --keep-others while a solve or a
    regression is running.
  * deathfx=0: the death effect crashes a spectated run.
"""

from __future__ import annotations

import argparse
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

from gdtas import observe, plan
from gdtas.paths import BUILD_MOD, DATA, WORKERS_ROOT


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--level", type=int, default=16)
    ap.add_argument("--plan", default="")
    ap.add_argument("--worker-id", type=int, default=91)
    ap.add_argument("--slowmo", type=int, default=1)
    ap.add_argument("--pause-at-x", type=float, default=0.0,
                    help="freeze the picture at this x instead of deriving one "
                         "from the plan's .dump. For watching one spot (a coin "
                         "the game refused, a death) rather than the plan's end")
    ap.add_argument("--cfg", nargs="*", default=[],
                    help="extra autorun.cfg lines, k=v. `coins=1 coinmode=1` "
                         "makes the coin lines print; leave coinroute OFF for a "
                         "watch, it ends the attempt at a coin the plan missed")
    ap.add_argument("--pause-before", type=float, default=40.0,
                    help="how many px before the plan runs out to freeze the "
                         "picture. 0 = do not freeze; without it GD's own retry "
                         "starts replaying with no input, which is confusing")
    ap.add_argument("--resolution-index", type=int, default=25)
    ap.add_argument("--itermap", action="store_true",
                    help="draw the iteration map over the level from the start "
                         "(otherwise F10 toggles it). Needs "
                         "data/itermap_lv{N}.txt -- build one from a run's log "
                         "with py/itermap_from_log.py")
    ap.add_argument("--keep-others", action="store_true",
                    help="do not kill the other workers (when a solve or a "
                         "regression is running alongside)")
    ap.add_argument("--workers-root", default=str(WORKERS_ROOT))
    ap.add_argument("--mod", default=str(BUILD_MOD))
    a = ap.parse_args(argv)

    plan_path = Path(a.plan) if a.plan else DATA / f"solution_lv{a.level}_dp.txt.best"
    if not plan_path.exists():
        print(f"plan not found: {plan_path}")
        return 1
    inputs = plan.read_input_lines(plan_path)

    # Where to stop the picture: just short of how far this plan actually gets in GD
    pause_at = a.pause_at_x
    dump = Path(str(plan_path) + ".dump")
    if pause_at <= 0 and a.pause_before > 0 and dump.exists():
        tail = dump.read_text(encoding="utf-8-sig", errors="replace").splitlines()
        if tail:
            last_x = float(tail[-1].split(",")[3])
            if last_x > a.pause_before:
                pause_at = last_x - a.pause_before

    # The map the F10 overlay reads. It is a side file the session opens by name, so it has to
    # travel to the worker's data root with the plan; without it the overlay says there is
    # nothing recorded for this level, which is true of that worker and misleading about the run.
    # Carried whenever there is one, so F10 works during any replay; --itermap only decides
    # whether it starts drawn.
    extra: list[Path] = []
    itermap_path = DATA / f"itermap_lv{a.level}.txt"
    if itermap_path.exists():
        extra.append(itermap_path)
    elif a.itermap:
        print(f"no iteration map: {itermap_path}\n"
              f"  build one with: python py/itermap_from_log.py "
              f"data/coldlog_lv{a.level}.txt")
        return 1

    cfg = [
        "enabled=1", f"level={a.level}",
        "solve=0",          # replay, not search
        "blockinput=1",     # the plan drives; the keyboard is ignored
        "attempts=1", "quitwhendone=0",
        "skiprender=0",     # draw it
        "fastloops=1",      # one physics batch per frame = real time
        "fastdt=0",
        "deathfx=0",        # see the note at the top
        "notrace=1", "cbs=0", "cos=1",
        f"pauseatx={pause_at}", f"slowmo={a.slowmo}",
    ] + (["itermap=1"] if a.itermap else []) + list(a.cfg) + inputs

    note = f"  (pauses at x={int(pause_at)})" if pause_at > 0 else ""
    print(f"lv{a.level}: {len(inputs)} inputs from {plan_path.name}{note}"
          + ("  + iteration map (F10)" if extra else ""))

    observe.launch_visible(a.worker_id, cfg, a.resolution_index, Path(a.mod),
                           kill_others=not a.keep_others,
                           workers_root=Path(a.workers_root),
                           extra_files=extra)
    print(f"watching lv{a.level} at {a.slowmo}x-slow. Close the window when done.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
