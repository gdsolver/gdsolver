"""Limits a cold runner enforces inside its own launches.

A launch is otherwise bounded only by its wall-clock budget, which for a one-session
suite is the whole suite's: one level that loses its way can hold a worker -- and the
machine's memory -- for hours before anything stops it. The guard here is consulted
once a poll by gdtas.worker.run_session and ends that launch (only the processes started
from under the worker's own directory) when

  * the level in play has run past its deadline, or
  * the machine's available physical memory is below a floor.

It also keeps what is needed to say where the level was when it was stopped: the last
`[fp]` iteration line, the last death line, the last coin line, the last solver argv,
and the child's working set. Samples of free memory and the child's working set are
appended to a JSON-lines file so the run leaves a record even when nothing is stopped.

The deadline comes from the caller. cold_regress.py picks it from the previous run's
longest level times a factor (see `pick_deadline`), or takes it from the command line.
"""
from __future__ import annotations

import json
import re
import threading
import time
from pathlib import Path

from gdtas import procscan

FP_LINE = re.compile(r"\[fp\] it=\d+.*")
DEATH_LINE = re.compile(r"^(?:killer: |death: ).*", re.M)
COIN_LINE = re.compile(r"^coin(?:gd)?: .*", re.M)
ARGS_LINE = re.compile(r"dpsolve: solver args: .*")
SUITE_MARK = re.compile(r"^suite: level=(\d+) \(\d+/\d+\)$", re.M)


def _available_mb() -> float:
    try:
        import psutil
        return psutil.virtual_memory().available / (1024 * 1024)
    except Exception:            # noqa: BLE001  no reading is not a reason to stop
        return float("inf")


def _working_set_mb(worker_root: Path) -> float:
    """The summed working set of the processes started from under the worker's directory."""
    try:
        import psutil
        root = str(worker_root).lower()
        total = 0
        for _p, info in procscan.scan(["exe", "memory_info"]):
            exe = (info.get("exe") or "").lower()
            if exe.startswith(root) and info.get("memory_info"):
                total += info["memory_info"].rss
        return total / (1024 * 1024)
    except Exception:            # noqa: BLE001
        return -1.0


class LevelGuard:
    """One per launch. `progress(lines)` feeds it the log as it grows; calling it with the
    worker root decides whether to stop. For a suite, the level clock restarts at each
    `suite: level=` marker; for a single level it starts at construction."""

    def __init__(self, deadline_s: float, mem_floor_mb: float, sample_path: Path | None,
                 first_level: int | None = None, sample_every_s: float = 30.0):
        self.deadline_s = float(deadline_s)
        self.mem_floor_mb = float(mem_floor_mb)
        self.sample_path = sample_path
        self.sample_every_s = sample_every_s
        self.level = first_level
        self.level_t0 = time.time()
        self.last = {"fp": "", "death": "", "coin": "", "argv": ""}
        self.peak_ws_mb = 0.0
        self.last_ws_mb = -1.0
        self.last_free_mb = -1.0
        self._next_sample = 0.0
        self.stopped = None          # the snapshot taken when it said stop

    def progress(self, lines: str) -> None:
        for m in SUITE_MARK.finditer(lines):
            self.level = int(m.group(1))
            self.level_t0 = time.time()
        for key, rx in (("fp", FP_LINE), ("death", DEATH_LINE), ("coin", COIN_LINE),
                        ("argv", ARGS_LINE)):
            found = rx.findall(lines)
            if found:
                self.last[key] = found[-1][:4000]

    def _sample(self, worker_root: Path) -> None:
        now = time.time()
        self.last_free_mb = _available_mb()
        if now < self._next_sample:
            return
        self._next_sample = now + self.sample_every_s
        self.last_ws_mb = _working_set_mb(worker_root)
        self.peak_ws_mb = max(self.peak_ws_mb, self.last_ws_mb)
        if self.sample_path is not None:
            try:
                with open(self.sample_path, "a", encoding="utf-8") as f:
                    f.write(json.dumps({"t": round(now, 1), "level": self.level,
                                        "level_s": round(now - self.level_t0, 1),
                                        "free_mb": round(self.last_free_mb),
                                        "ws_mb": round(self.last_ws_mb)}) + "\n")
            except OSError:
                pass

    def snapshot(self, why: str) -> dict:
        return {"why": why, "level": self.level,
                "level_s": round(time.time() - self.level_t0, 1),
                "deadline_s": self.deadline_s, "mem_floor_mb": self.mem_floor_mb,
                "free_mb": round(self.last_free_mb), "ws_mb": round(self.last_ws_mb),
                "peak_ws_mb": round(self.peak_ws_mb),
                "at": time.strftime("%Y-%m-%d %H:%M:%S"), **self.last}

    def __call__(self, worker_root: Path) -> str:
        self._sample(worker_root)
        why = ""
        if self.deadline_s > 0 and time.time() - self.level_t0 > self.deadline_s:
            why = f"level deadline: lv{self.level} past {self.deadline_s:.0f}s"
        elif self.mem_floor_mb > 0 and self.last_free_mb < self.mem_floor_mb:
            why = (f"memory floor: {self.last_free_mb:.0f} MB available, "
                   f"under {self.mem_floor_mb:.0f} MB")
        if why:
            self.last_ws_mb = _working_set_mb(worker_root)
            self.peak_ws_mb = max(self.peak_ws_mb, self.last_ws_mb)
            self.stopped = self.snapshot(why)
        return why

    @property
    def memory_stop(self) -> bool:
        return bool(self.stopped) and self.stopped["why"].startswith("memory floor")


def pick_deadline(prev_manifest: Path, scale: float, floor_s: float, cap_s: float,
                  given: float | None) -> tuple[float, str]:
    """(seconds, where the number came from). A value on the command line wins. Otherwise
    the longest level of the previous run in that directory (its manifest's `walls`,
    written by `note_walls`), times `scale`, kept between `floor_s` and `cap_s`. With no
    previous walls, `cap_s` -- the per-level budget, i.e. the old behaviour."""
    if given:
        return float(given), "command line"
    try:
        m = json.loads(prev_manifest.read_text(encoding="utf-8"))
        walls = [float(v) for v in (m.get("walls") or {}).values() if v]
    except (OSError, ValueError, TypeError):
        walls = []
    if not walls:
        return float(cap_s), "per-level budget (no previous walls)"
    d = min(cap_s, max(floor_s, max(walls) * scale))
    return d, f"previous run's longest level {max(walls):.0f}s x {scale:g}"


_MANIFEST_LOCK = threading.Lock()   # the parallel arrangement runs one() in threads


def _update_manifest(out_dir: Path, fn) -> None:
    p = out_dir / "cold_manifest.json"
    with _MANIFEST_LOCK:
        try:
            m = json.loads(p.read_text(encoding="utf-8"))
        except (OSError, ValueError):
            return
        fn(m)
        try:
            p.write_text(json.dumps(m, indent=1), encoding="utf-8")
        except OSError:
            pass


def note_guard(out_dir: Path, settings: dict) -> None:
    """The guard's settings, and an empty `stops` beside them: a run that stopped nothing
    then says so, where a missing key could also mean the stops were never recorded."""
    def put(m):
        m["guard"] = settings
        m.setdefault("stops", {})
    _update_manifest(out_dir, put)


def note_stop(out_dir: Path, level: int, snap: dict) -> None:
    _update_manifest(out_dir, lambda m: m.setdefault("stops", {}).__setitem__(str(level), snap))


def note_walls(out_dir: Path, walls: dict[int, float]) -> None:
    def put(m):
        w = m.setdefault("walls", {})
        for lv, s in walls.items():
            w[str(lv)] = round(float(s), 1)
    _update_manifest(out_dir, put)
