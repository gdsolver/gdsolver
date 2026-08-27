"""Turn a GD worker's window into a PNG.

    python py/screenshot_worker.py --worker-id 97 --out data/shot.png

Why it is needed: a solve runs with skiprender=1, so the screen shows nothing but
the HUD. To judge a wall by eye there is no alternative to replaying the best plan
at 1x with the drawing on, freezing it at the wall (cfg pauseatx=<x>) and looking.
The numbers only say where it dies; only the picture says what it dies on.
"""

from __future__ import annotations

import argparse
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

from gdtas import window


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--worker-id", required=True, type=int)
    ap.add_argument("--out", required=True)
    a = ap.parse_args(argv)

    w, h = window.capture_worker(a.worker_id, Path(a.out))
    print(f"saved {a.out} ({w} x {h})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
