"""Run the section-limited GD solver over exactly one section.

  python py/secsolve_run.py <worker> <level> <plan|-> <startTick> <targetX>
                            [horizon] [cap] [key=val ...]

Given a plan, that plan is replayed to the head of the section (this is
observation, so it does not break the cold rule). Pass "-" to get as far as no
input at all reaches.

Any trailing key=val is added to the cfg as it is (`secyq=0.5 secvq=0.25` and so
on). `seclog=1` is on by default -- without the per-layer breakdown there is no
telling "the cap thinned it out" from "there really was no way on".
"""
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from gdtas import plan as P                                # noqa: E402
from gdtas.paths import WORKERS_ROOT                       # noqa: E402
from gdtas.worker import run_session                       # noqa: E402

w = int(sys.argv[1])
level = int(sys.argv[2])
planp = sys.argv[3]
start = int(sys.argv[4])
target = float(sys.argv[5])
rest = sys.argv[6:]
extra = [a for a in rest if "=" in a]
pos = [a for a in rest if "=" not in a]
horizon = int(pos[0]) if len(pos) > 0 else 120
cap = int(pos[1]) if len(pos) > 1 else 40

lines = P.read_input_lines(Path(planp)) if planp != "-" else []
cfg = ["enabled=1", f"level={level}", "attempts=1", "quitwhendone=1",
       "blockinput=1", "cbs=0", "cos=1", "fastdt=0.0166667",
       # A checkpoint can only be dropped on a frame boundary, so fastloops is
       # what the tick granularity comes down to. At 1 every tick is a boundary:
       # the replay to the section head is slower, but it lands where asked.
       "fastloops=1", "skiprender=1", "solve=0", "music=mute", "coins=0",
       "notrace=1",
       f"practiceat={max(1, start - 100)}", f"checkpointat={start}",
       "secsolve=1", "seclog=1", f"secstart={start}", f"sectarget={target}",
       f"sechorizon={horizon}", f"seccap={cap}"] + extra + lines
res = run_session(w, cfg, timeout_s=3600, workers_root=WORKERS_ROOT)
layers = []
for l in res.lines:
    if "seclayer:" in l:
        layers.append(l.strip())
    elif any(k in l for k in ("secsolve", "secverify", "psnap", "checkpoint",
                              "practice_on", "death:", "stall", "complete")):
        print(l[:400])
# Not every layer is printed: the last 30, and where the frontier starts thinning.
if layers:
    print(f"--- seclayer: {len(layers)} layers (last 30) ---")
    for l in layers[-30:]:
        print(l[-160:])
