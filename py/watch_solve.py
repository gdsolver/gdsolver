"""Print one line of running status, over and over.

The section solver runs **entirely inside one GD frame**, so for as long as it
runs there is no drawing and no HUD refresh: the overlay looks frozen, for twenty
or thirty minutes. Nothing else prints either. All this does is watch the
worker's result.txt and the log from outside.

  python watch_solve.py [--worker 90] [--log data/solvelog_lv20_vr.txt]
                        [--interval 10]
"""
from __future__ import annotations
import argparse
import re
import time
from pathlib import Path


def tail_match(path: Path, pat: str, n: int = 1) -> list[str]:
    if not path.exists():
        return []
    try:
        lines = path.read_text(encoding="utf-8", errors="ignore").splitlines()
    except OSError:
        return []
    hit = [l for l in lines if pat in l]
    return hit[-n:]


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--worker", type=int, default=90)
    ap.add_argument("--workers-root", default=r"C:\GD-workers")
    ap.add_argument("--log", default=str(Path(__file__).resolve().parent.parent / "data" / "solvelog_lv20_vr.txt"))
    ap.add_argument("--interval", type=float, default=10.0)
    a = ap.parse_args()

    res = Path(a.workers_root) / f"worker-{a.worker}" / "session" / "data" / "result.txt"
    log = Path(a.log)
    t0 = time.time()
    prev_d, prev_t = None, None
    while True:
        el = int(time.time() - t0)
        drv = tail_match(log, "iter ") + tail_match(log, "section solver:")
        phase = drv[-1].strip()[:90] if drv else "(no log yet)"
        lay = tail_match(res, "seclayer: d=")
        rate = ""
        if lay:
            m = re.search(r"d=(\d+).*?x=([\d.]+).*?keep=(\d+)", lay[-1])
            if m:
                d, x, keep = int(m.group(1)), m.group(2), m.group(3)
                now = time.time()
                if prev_d is not None and d > prev_d:
                    rate = f" {(now - prev_t) / (d - prev_d):.1f}s/layer"
                prev_d, prev_t = d, now
                phase = f"{phase} | d={d} x={x} keep={keep}{rate}"
        fix = tail_match(res, "secfix:")
        if fix:
            phase += " | " + fix[-1].strip()[fix[-1].find("secfix:"):][:60]
        print(f"[{el // 60:3d}m{el % 60:02d}s] {phase}", flush=True)
        time.sleep(a.interval)


if __name__ == "__main__":
    main()
