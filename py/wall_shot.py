"""Replay a plan at 1x, freeze it at the wall, and photograph it.

    python py/wall_shot.py --level 20 --plan data/solution_lv20_dp.txt.best \\
        --pause-at-x 12000 --worker-id 97

The numbers only say WHERE a run dies. WHAT it dies on -- which obstacle, which
lane, the shape of what is around it -- only the picture says. A solve runs with
skiprender=1 and draws nothing at all, so the wall has to be replayed again with
the drawing turned on.

The sequence: replay in real time (no fastdt, so every frame is drawn), stop the
game clock once the player passes pauseatx, then capture the window. It stays
frozen, so there is no hurry about the capture.
"""

from __future__ import annotations

import argparse
import sys
import threading
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

from gdtas import gdsave, plan, results, window
from gdtas.paths import DATA, WORKERS_ROOT, worker_dir, worker_manifest
from gdtas.worker import run_session, stop_worker_processes


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--level", required=True, type=int)
    ap.add_argument("--plan", required=True)
    ap.add_argument("--pause-at-x", required=True, type=float)
    ap.add_argument("--worker-id", type=int, default=97)
    ap.add_argument("--out", default="")
    ap.add_argument("--timeout-minutes", type=float, default=8.0)
    ap.add_argument("--resolution-index", type=int, default=None,
                    help="for when the window is too small to read; by default "
                         "the save's own value is kept")
    ap.add_argument("--keep-open", action="store_true",
                    help="leave GD frozen at the wall after the capture, for a "
                         "human to look at")
    ap.add_argument("--workers-root", default=str(WORKERS_ROOT))
    a = ap.parse_args(argv)

    out = Path(a.out) if a.out else DATA / f"wall_lv{a.level}.png"
    inputs = plan.read_input_lines(a.plan)
    if not inputs:
        print(f"no input= lines in {a.plan}")
        return 1

    cfg = [
        "enabled=1", f"level={a.level}", "attempts=1", "quitwhendone=0",
        "blockinput=1", "cbs=0", "cos=1", "notrace=1",
        "solve=0", "music=mute", f"pauseatx={a.pause_at_x}",
    ] + inputs

    workers_root = Path(a.workers_root)
    man = worker_manifest(a.worker_id, workers_root)
    data = Path(man["data_root"])
    image = Path(man["executable"]).name
    root = worker_dir(a.worker_id, workers_root)
    (data / "result.txt").unlink(missing_ok=True)
    if a.resolution_index is not None:
        gdsave.ensure_windowed(a.worker_id)
        gdsave.set_resolution_index(a.worker_id, a.resolution_index)

    timeout_s = a.timeout_minutes * 60
    # minimized=False is required: PrintWindow on a minimised window returns white.
    th = threading.Thread(
        target=run_session,
        args=(a.worker_id, cfg, timeout_s, workers_root),
        kwargs={"minimized": False}, daemon=True)
    th.start()

    paused = None
    deadline = time.time() + timeout_s
    while time.time() < deadline and th.is_alive():
        time.sleep(2.0)
        paused = results.first(results.read_lines(data / "result.txt"),
                               "pauseatx: paused")
        if paused:
            break

    if not paused:
        stop_worker_processes(root, image)
        print(f"never reached x={a.pause_at_x} "
              f"(does the plan die earlier? see {data / 'result.txt'})")
        return 1

    time.sleep(1.0)
    w, h = window.capture_worker(a.worker_id, out)
    print(f"saved {out} ({w} x {h})")
    print(paused)

    if a.keep_open:
        print(f"GD left frozen at the wall (worker-{a.worker_id}). "
              f"Close the window when done.")
        return 0
    stop_worker_processes(root, image)
    return 0


if __name__ == "__main__":
    sys.exit(main())
