"""Re-take data/objrects_lv<N>.txt and friends from a running GD.

    python py/refresh_objrects.py --levels 19 20 21 22 --worker-id 99

Why it is needed: a stored dump can shift by a fraction of a px between
sessions, and a difference of under 0.1px has genuinely produced a phantom
wall before. Re-take them before trusting a tight clearance.

The dumps are emitted by solver::buildPois. It only runs when asked, so the cfg
`clearance=1` is used PURELY AS A TRIGGER.
objrects is the geometry; triggers/objgroups are the trigger->group->uid map
that tells you "which wall is a door"; obb is the real shape of rotated
objects. All four come out of the same buildPois call, so their uids cannot
disagree (the maps are joined onto objrects by uid).
"""

from __future__ import annotations

import argparse
import shutil
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

from gdtas.paths import LEVEL_DATA, WORKERS_ROOT
from gdtas.worker import Worker, WorkerError

DUMPS = ("objrects.txt", "triggers.txt", "objgroups.txt", "obb.txt",
         "forceblocks.txt")


def refresh(level: int, worker_id: int, data_dir: Path, workers_root: Path,
            timeout_s: float = 120.0) -> bool:
    w = Worker(worker_id, workers_root)
    try:
        # DELETE THEM FIRST. The worker's data root still holds dumps from the
        # previous level and the previous build, and "adopt it if it exists"
        # without deleting grabs the stale table of another level (measured:
        # meaning to re-take lv1, we grabbed an 18k-line file of something else).
        for name in DUMPS:
            (w.data / name).unlink(missing_ok=True)
        w.open(level, {"clearance": "1"}, timeout_s=timeout_s)
        # the dumps are written when the level loads. There is no need to wait
        # for the run to land, but do wait for the write to finish (until the
        # size settles).
        deadline = time.time() + 60.0
        last = -1
        while time.time() < deadline:
            time.sleep(0.5)
            src = w.data / "objrects.txt"
            size = src.stat().st_size if src.exists() else -1
            if size > 0 and size == last:
                break
            last = size
        any_copied = False
        for name in DUMPS:
            src = w.data / name
            if not src.exists():
                continue
            any_copied = True
            dst = data_dir / name.replace(".txt", f"_lv{level}.txt")
            old = dst.stat().st_size if dst.exists() else 0
            shutil.copyfile(src, dst)
            print(f"lv{level}: {dst.name}  {old} -> {dst.stat().st_size} bytes")
        if not any_copied:
            print(f"lv{level}: NO DUMP - buildPois did not run")
        return any_copied
    finally:
        w.close()


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--levels", nargs="+", type=int, default=[19, 20, 21, 22])
    ap.add_argument("--worker-id", type=int, default=99)
    ap.add_argument("--data-dir", default=str(LEVEL_DATA))
    ap.add_argument("--workers-root", default=str(WORKERS_ROOT))
    a = ap.parse_args(argv)

    bad = 0
    for lv in a.levels:
        try:
            if not refresh(lv, a.worker_id, Path(a.data_dir), Path(a.workers_root)):
                bad += 1
        except WorkerError as e:
            print(f"lv{lv}: {e}")
            bad += 1
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
