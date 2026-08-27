"""Replay every stored solution headless and check that it really clears.

    python py/verify_solutions.py --files data/solution_lv*_dp.txt \\
        --parallel 6 --pool 90 91 92 93 94 95

GD's levelComplete is called mid-level as well, so a "clear" in the session
that solved the level is not evidence. The only thing to trust is a bare
replay with solve=0 blockinput=1.
The physics runs on a fixed 240Hz substep, so the result does not change when
parallel runs compete for CPU (only the wall-clock time does).
"""

from __future__ import annotations

import argparse
import re
import sys
from concurrent.futures import ThreadPoolExecutor
from dataclasses import dataclass
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

from gdtas import memory, plan, results
from gdtas.paths import VERIFY_RESULT, WORKERS_ROOT
from gdtas.worker import run_session


def level_id(name: str) -> int:
    """Level number from the file name. Also picks up variants like solution_vf_lv11.txt."""
    m = re.search(r"lv(\d+)", name)
    if m:
        return int(m.group(1))
    return 1 if "solution_sm" in name else 0


@dataclass
class Verdict:
    file: str
    level: int
    clear: bool
    detail: str
    worker: int


def verify_one(worker_id: int, path: Path, level: int, timeout_s: float,
               workers_root: Path) -> Verdict:
    # a coin solution claims "clear + all coins", so make it report the count too.
    # the coin gate is inside solve=1, so coins=1 does not change a bare replay.
    coins = "coins=1" if "_coins" in path.name else "coins=0"
    cfg = [
        "enabled=1", f"level={level}", "attempts=1", "quitwhendone=1",
        "blockinput=1", "cbs=0", "cos=1", "notrace=1",
        "fastdt=0.0166667", "fastloops=1800", "skiprender=1",
        "solve=0", "music=mute", coins,
    ] + plan.read_input_lines(path)
    r = run_session(worker_id, cfg, timeout_s=timeout_s, workers_root=workers_root)
    clear, detail = results.clear_verdict(r.lines)
    if r.timed_out and not clear:
        detail = f"TIMEOUT ({detail})"
    return Verdict(path.name, level, clear, detail or "", worker_id)


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--files", nargs="+", required=True)
    ap.add_argument("--parallel", type=int, default=8)
    ap.add_argument("--timeout-minutes", type=float, default=6.0)
    ap.add_argument("--workers-root", default=str(WORKERS_ROOT))
    ap.add_argument("--out", default=str(VERIFY_RESULT))
    # which workers to use. A long measurement may already be holding a few of
    # them, and sharing a data root is refused, so this has to be selectable.
    ap.add_argument("--pool", nargs="+", type=int,
                    default=[90, 91, 92, 93, 94, 95, 96, 97])
    ap.add_argument("--no-free-memory", action="store_true")
    a = ap.parse_args(argv)

    workers_root = Path(a.workers_root)
    pool = a.pool[:max(1, a.parallel)]

    work = []
    for f in a.files:
        p = Path(f)
        lv = level_id(p.name)
        if lv == 0:
            print(f"skip (no level id): {f}")
            continue
        work.append((p, lv))
    if not work:
        print("nothing to verify")
        return 2
    print(f"verifying {len(work)} solutions on {len(pool)} workers")

    if not a.no_free_memory:
        memory.clear_steam_webhelper()

    # hand them out round-robin. One worker runs one at a time in order, so the
    # weight of the levels does not pile up on one side.
    buckets: dict[int, list] = {w: [] for w in pool}
    for i, item in enumerate(work):
        buckets[pool[i % len(pool)]].append(item)

    timeout_s = a.timeout_minutes * 60

    def run_bucket(w: int) -> list[Verdict]:
        out = []
        for p, lv in buckets[w]:
            try:
                out.append(verify_one(w, p, lv, timeout_s, workers_root))
            except Exception as e:                       # noqa: BLE001
                out.append(Verdict(p.name, lv, False, f"ERROR {e}", w))
        return out

    verdicts: list[Verdict] = []
    with ThreadPoolExecutor(max_workers=len(pool)) as ex:
        for res in ex.map(run_bucket, [w for w in pool if buckets[w]]):
            verdicts.extend(res)

    lines = [f"solution replay verification ({len(verdicts)} files)"]
    for v in sorted(verdicts, key=lambda v: (v.level, v.file)):
        lines.append(f"{v.level:<4} {v.file:<40} {'OK' if v.clear else 'FAIL':<5} "
                     f"{v.detail}")
    fails = sum(1 for v in verdicts if not v.clear)
    lines.append(f"--- {len(verdicts) - fails} OK / {fails} FAIL ---")
    Path(a.out).write_text("\n".join(lines) + "\n", encoding="utf-8")
    print("\n".join(lines))
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
