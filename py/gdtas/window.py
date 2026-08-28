"""Finding, waiting for and capturing a worker's window (win32).

Trap: PrintWindow on a minimised window returns a pure white bitmap. A worker meant to
be captured must be launched non-minimised (minimized=False in worker._spawn).

Trap: DPI. Measured 2026-08-28 on a 144-DPI (150%) display -- a worker window whose real
client area is 2560x1440 reports 1706x960 to a DPI-unaware process, and PrintWindow then
captures only the top-left 1706x960 of it. Every screenshot was silently cropped to two
thirds of the window, and anything drawn in the bottom-right corner was invisible in the
picture while being perfectly fine on screen. (The note in src/mod/hud.hpp about the right
and bottom of getWinSize() not being reliably on screen came from this same instrument.)
So: declare DPI awareness before touching any window API.
"""

from __future__ import annotations

import ctypes
import time
from pathlib import Path

import win32gui
import win32process
import win32ui

from .paths import worker_dir
from .worker import processes_under

# PW_RENDERFULLCONTENT. Without it, GL/DX surfaces come back pure black
PW_RENDERFULLCONTENT = 2


def _declare_dpi_aware() -> None:
    """Ask for real pixels, newest API first. Failure is not fatal: without it every
    measurement here is scaled by the display's DPI factor, which is wrong but not
    broken, and on a 100% display there is no difference at all."""
    # DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2 (Windows 10 1703+)
    try:
        if ctypes.windll.user32.SetProcessDpiAwarenessContext(-4):
            return
    except Exception:  # noqa: BLE001 - older Windows has no such export
        pass
    try:
        ctypes.windll.shcore.SetProcessDpiAwareness(2)   # PER_MONITOR_DPI_AWARE
        return
    except Exception:  # noqa: BLE001
        pass
    try:
        ctypes.windll.user32.SetProcessDPIAware()
    except Exception:  # noqa: BLE001
        pass


_declare_dpi_aware()


def windows_of_pid(pid: int) -> list[int]:
    found: list[int] = []

    def cb(hwnd, _):
        if not win32gui.IsWindowVisible(hwnd):
            return True
        if win32gui.GetParent(hwnd):
            return True
        _, wpid = win32process.GetWindowThreadProcessId(hwnd)
        if wpid == pid:
            found.append(hwnd)
        return True

    win32gui.EnumWindows(cb, None)
    return found


def worker_hwnd(worker_id: int, workers_root: Path | None = None) -> int:
    """The main window of the GD launched from under that worker directory."""
    root = worker_dir(worker_id) if workers_root is None else \
        Path(workers_root) / f"worker-{worker_id}"
    for pid in processes_under(root):
        hs = windows_of_pid(pid)
        if hs:
            return hs[0]
    return 0


def wait_for_window(pid: int, timeout_s: float = 30.0) -> int:
    deadline = time.time() + timeout_s
    while time.time() < deadline:
        hs = windows_of_pid(pid)
        if hs:
            return hs[0]
        time.sleep(0.5)
    return 0


def set_foreground(hwnd: int) -> None:
    """DO NOT CALL THIS RIGHT AFTER LAUNCH: it has been measured to race with window
    creation and freeze the worker before it advances a single frame. Wait a few
    seconds after the window has appeared.
    """
    try:
        win32gui.SetForegroundWindow(hwnd)
    except Exception:                                    # noqa: BLE001
        pass


def client_size(hwnd: int) -> tuple[int, int]:
    l, t, r, b = win32gui.GetClientRect(hwnd)
    return r - l, b - t


def capture_window(hwnd: int, out_path: Path | str) -> tuple[int, int]:
    """Capture the window to a PNG. Returns (w, h)."""
    l, t, r, b = win32gui.GetWindowRect(hwnd)
    w, h = r - l, b - t
    if w <= 0 or h <= 0:
        raise RuntimeError("window has no area (minimized?)")

    win_dc = win32gui.GetWindowDC(hwnd)
    src = win32ui.CreateDCFromHandle(win_dc)
    dst = src.CreateCompatibleDC()
    bmp = win32ui.CreateBitmap()
    bmp.CreateCompatibleBitmap(src, w, h)
    dst.SelectObject(bmp)
    try:
        ok = ctypes.windll.user32.PrintWindow(hwnd, dst.GetSafeHdc(),
                                              PW_RENDERFULLCONTENT)
        if not ok:
            raise RuntimeError("PrintWindow failed")
        info = bmp.GetInfo()
        bits = bmp.GetBitmapBits(True)
        from PIL import Image
        img = Image.frombuffer("RGB", (info["bmWidth"], info["bmHeight"]),
                               bits, "raw", "BGRX", 0, 1)
        out = Path(out_path)
        out.parent.mkdir(parents=True, exist_ok=True)
        img.save(out, "PNG")
        return info["bmWidth"], info["bmHeight"]
    finally:
        win32gui.DeleteObject(bmp.GetHandle())
        dst.DeleteDC()
        src.DeleteDC()
        win32gui.ReleaseDC(hwnd, win_dc)


def capture_worker(worker_id: int, out_path: Path | str) -> tuple[int, int]:
    hwnd = worker_hwnd(worker_id)
    if not hwnd:
        raise RuntimeError(f"worker-{worker_id} is not running (or has no window)")
    return capture_window(hwnd, out_path)


__all__ = ["windows_of_pid", "worker_hwnd", "wait_for_window", "set_foreground",
           "client_size", "capture_window", "capture_worker"]
