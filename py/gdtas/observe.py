"""Launching a worker for a human to watch (visible, foreground, real-time speed).

Unlike a headless run, this one needs a window size and focus:
  * The window size lives in the worker's own save, and GD restores the saved value
    one second after the window appears, so it can only be written BEFORE THE PROCESS
    STARTS (gdsave).
  * SetForegroundWindow right after launch races with window creation and freezes the
    worker. Call it a few seconds after the window has appeared.
"""

from __future__ import annotations

import subprocess
import time
from pathlib import Path

from . import gdsave, window
from .paths import BUILD_MOD, WORKERS_ROOT
from .worker import (_spawn, _WorkerBase, taskkill_image)


def kill_all_gd() -> None:
    """Kill every GeometryDash* process (including the user's real GD).

    A worker being watched needs the foreground and its own resolution, so this is
    what we do by default. Do not call it while another solve or regression is running.
    """
    import psutil
    for p in psutil.process_iter(["pid", "name"]):
        if (p.info["name"] or "").lower().startswith("geometrydash"):
            try:
                p.kill()
            except psutil.Error:
                pass
    for _ in range(40):
        alive = [p for p in psutil.process_iter(["name"])
                 if (p.info["name"] or "").lower().startswith("geometrydash")]
        if not alive:
            return
        time.sleep(0.25)


def launch_visible(worker_id: int, cfg_lines: list[str],
                   resolution_index: int = 25, mod_file: Path = BUILD_MOD,
                   kill_others: bool = False, workers_root: Path = WORKERS_ROOT,
                   foreground: bool = True) -> tuple[subprocess.Popen, Path]:
    """Start one session with a visible window. Returns (process, data_root).

    cfg_lines is the body of autorun.cfg verbatim (`input=` lines may be mixed in).
    """
    if kill_others:
        kill_all_gd()

    w = _WorkerBase(worker_id, workers_root)
    taskkill_image(w.image)          # this worker's leftovers must always be removed
    w._wait_free()
    w._prepare(mod_file)

    for name in ("result.txt", "trace.csv", "dump.csv", "cmd.txt", "plan_in.txt"):
        (w.data / name).unlink(missing_ok=True)
    (w.data / "autorun.cfg").write_bytes(("\n".join(cfg_lines) + "\n").encode("ascii"))

    gdsave.ensure_windowed(worker_id)
    gdsave.set_resolution_index(worker_id, resolution_index)

    proc = _spawn(w.exe, w.root, w.data, minimized=False)
    print(f"worker-{worker_id} PID: {proc.pid}  data: {w.data}")

    hwnd = window.wait_for_window(proc.pid, 30.0)
    if not hwnd:
        print("no main window after 30s")
        return proc, w.data
    time.sleep(2.0)   # GD re-applies the saved size right after the window appears
    cw, ch = window.client_size(hwnd)
    ratio = cw / ch if ch else 0.0
    print(f"client area: {cw}x{ch}  ratio {ratio:.3f} (16:9 = 1.778)")
    if foreground:
        window.set_foreground(hwnd)
    return proc, w.data
