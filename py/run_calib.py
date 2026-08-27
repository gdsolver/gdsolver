# -*- coding: utf-8 -*-
"""Run a calibration map once (with its generated plan if present)."""
import shutil
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from gdtas import plan as P  # noqa: E402
from gdtas.worker import run_session  # noqa: E402
from gdtas.paths import RIGS, WORKERS_ROOT  # noqa: E402

lvl = Path(sys.argv[1]) if len(sys.argv) > 1 else RIGS / "calib_probe.lvl"
wid = int(sys.argv[2]) if len(sys.argv) > 2 else 99
extra = sys.argv[3:] if len(sys.argv) > 3 else []
dst = WORKERS_ROOT / f"worker-{wid}" / "session" / "data"
dst.mkdir(parents=True, exist_ok=True)
shutil.copy2(lvl, dst / lvl.name)

cfg = [
    "enabled=1", "level=9001", f"levelfile={dst / lvl.name}",
    "attempts=1", "quitwhendone=1", "blockinput=1",
    "fastdt=0.0166667", "fastloops=600", "skiprender=1",
    "solve=0", "music=mute", "coins=0", "resolution=25",
] + list(extra)
plan = lvl.with_suffix(".plan.txt")
if plan.exists():
    cfg += P.read_input_lines(plan)
    print(f"plan: {plan.name}")
res = run_session(wid, cfg, timeout_s=300, workers_root=WORKERS_ROOT)
print("timed_out:", res.timed_out)
for l in res.lines:
    if any(k in l for k in ("death", "complete", "session_end", "error",
                            "loaded")):
        print(" ", l)
