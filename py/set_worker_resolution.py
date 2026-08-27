"""Set the window resolution index of a worker's GD.

    python py/set_worker_resolution.py --worker-id 97 --index 21

Why it is needed: wall_shot.py photographs the worker's window. A fresh worker
save has no resolution key, so GD comes up at about 426x320, where there is no
telling what the obstacle even is. The index lives in the worker's own save, so
it has to be written before the process starts.

The measured table and its encoding are all in gdtas/gdsave.py.
"""

from __future__ import annotations

import argparse
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

from gdtas import gdsave


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(
        description=__doc__.splitlines()[0],
        epilog="index: 21=1280x720(16:9) 22=1280x800 23=1280x960 "
               "24=1365x1024 25=1706x960(16:9)")
    ap.add_argument("--worker-id", required=True)
    ap.add_argument("--index", type=int, default=21)
    ap.add_argument("--windowed", action="store_true",
                    help="also force windowed and non-borderless")
    a = ap.parse_args(argv)

    primary, _ = gdsave.save_paths(a.worker_id)
    if not primary.exists():
        print(f"no save for worker-{a.worker_id} ({primary})")
        return 1
    if a.windowed:
        gdsave.ensure_windowed(a.worker_id)
    gdsave.set_resolution_index(a.worker_id, a.index)
    print(f"worker-{a.worker_id} resolution index -> {a.index}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
