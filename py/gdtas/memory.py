"""Reclaiming memory before launching GD workers.

    python -m gdtas.memory

steamwebhelper (Steam's built-in browser) grows without any ceiling (tens of GB
measured), so if left alone the GD workers die at launch. Killing it gives the memory
back, and Steam immediately respawns it, so the client is unharmed = the worker launch
path stays alive.
It grows again, so this is not a one-off fix but a chore the batch calls every time.
"""

from __future__ import annotations

import argparse
import sys
import time

MB = 1024 * 1024


def clear_steam_webhelper(max_working_set_mb: int = 3000,
                          min_free_mb: int = 4000) -> dict:
    """Kill the helpers if their total exceeds the limit, or free physical memory is below it."""
    import psutil
    procs = [p for p in psutil.process_iter(["pid", "name"])
             if (p.info["name"] or "").lower() == "steamwebhelper.exe"]
    if not procs:
        return {"killed": 0}
    ws_mb = 0
    for p in procs:
        try:
            ws_mb += p.memory_info().rss // MB
        except psutil.Error:
            pass
    free_mb = psutil.virtual_memory().available // MB
    if ws_mb <= max_working_set_mb and free_mb >= min_free_mb:
        print(f"memory ok: free {free_mb} MB, steamwebhelper {ws_mb} MB")
        return {"killed": 0, "free_mb": free_mb, "ws_mb": ws_mb}
    print(f"reclaiming: free {free_mb} MB, steamwebhelper {ws_mb} MB "
          f"across {len(procs)} processes")
    killed = 0
    for p in procs:
        try:
            p.kill()
            killed += 1
        except psutil.Error:
            pass
    time.sleep(5)
    after = psutil.virtual_memory().available // MB
    print(f"  free {free_mb} MB -> {after} MB")
    return {"killed": killed, "free_mb": after, "ws_mb": ws_mb}


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(prog="gdtas.memory")
    ap.add_argument("--max-ws-mb", type=int, default=3000)
    ap.add_argument("--min-free-mb", type=int, default=4000)
    a = ap.parse_args(argv)
    clear_steam_webhelper(a.max_ws_mb, a.min_free_mb)
    return 0


if __name__ == "__main__":
    sys.exit(main())
