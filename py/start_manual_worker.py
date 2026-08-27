"""Start a GD worker for a human to play, in a 16:9 window that records the input.

    python py/start_manual_worker.py --worker-id 91 --level 3

Why it is needed: the Steam copy refuses to start while another GD is alive, so a
human demo needs a worker instance of its own. This is the recording counterpart
of the headless sessions (verify_solutions and friends).

  solve=0 blockinput=0  -> a human drives; nothing is injected
  notrace=0             -> handleButton lands in data/trace.csv with tick numbers
  attempts=999999       -> the session survives any number of deaths
  quitwhendone=0        -> GD stays open after a clear

slowmo N runs the game at 1/N speed, for demonstrating a hard part. It only
divides the dt handed to GD, so the 1/240s substeps and the tick numbers do not
change: a run recorded slowly replays as it is at 1x. It can only be set by this
argument at launch. To change speed during play, use the spectating speed on the
arrow keys, which does not touch dt.
"""

from __future__ import annotations

import argparse
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

from gdtas import observe
from gdtas.paths import BUILD_MOD, WORKERS_ROOT


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--worker-id", type=int, default=91)
    ap.add_argument("--level", type=int, default=3)
    ap.add_argument("--slowmo", type=int, default=1)
    ap.add_argument("--resolution-index", type=int, default=25,
                    help="25=1706x960 (16:9); the table is in gdtas/gdsave.py")
    ap.add_argument("--panel", action="store_true",
                    help="no autorun: leave the game on its menu and let the on-screen "
                         "panel drive (recording / demo). --level is then ignored")
    ap.add_argument("--kill-others", action="store_true",
                    help="kill every other GD before starting")
    ap.add_argument("--workers-root", default=str(WORKERS_ROOT))
    ap.add_argument("--mod", default=str(BUILD_MOD))
    a = ap.parse_args(argv)

    if a.panel:
        # The mod does nothing at all until the panel starts a session, which is the point:
        # what is being filmed is the panel, the mode switch and the level being picked out
        # of the game's own UI. uiConfigureSession (session.hpp) writes the whole session
        # config itself when a level is entered -- input precision included -- so nothing
        # here has to be right, and a session started this way is cold either way: the loop
        # deletes the previous run's fixups and recordings before it solves.
        cfg = ["enabled=0"]
    else:
        cfg = [
            "enabled=1", f"level={a.level}",
            "attempts=999999", "quitwhendone=0",
            "solve=0", "blockinput=0", "notrace=0",
            "cbs=0", "cos=1", f"slowmo={a.slowmo}",
        ]
    _, data = observe.launch_visible(a.worker_id, cfg, a.resolution_index,
                                     Path(a.mod), kill_others=a.kill_others,
                                     workers_root=Path(a.workers_root))
    print(data)
    return 0


if __name__ == "__main__":
    sys.exit(main())
