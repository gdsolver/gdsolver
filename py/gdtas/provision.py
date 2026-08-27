"""Cloning GD workers.

    python -m gdtas.provision --worker-id 98

A worker is "a renamed GD + a dedicated Geode root + a dedicated save + a dedicated
data root". Resources is a junction to the original GD, so the real size is ~80MB.
"""

from __future__ import annotations

import argparse
import json
import shutil
import subprocess
import sys
from datetime import datetime, timezone
from pathlib import Path

from .paths import WORKERS_ROOT, gd_save_root, worker_manifest
from .worker import CREATE_NO_WINDOW, WorkerError, processes_under, repair_save


def _junction(link: Path, target: Path) -> None:
    # Python has no API for creating a junction. mklink /J is a cmd built-in command.
    r = subprocess.run(["cmd", "/c", "mklink", "/J", str(link), str(target)],
                       capture_output=True, text=True, errors="replace",
                       creationflags=CREATE_NO_WINDOW)
    if r.returncode != 0 or not link.exists():
        raise WorkerError(f"mklink /J failed: {r.stdout}{r.stderr}")


def create_worker(worker_id: int, from_worker_id: int = 96,
                  workers_root: Path = WORKERS_ROOT,
                  min_worker_id: int = 0) -> Path:
    """Create a new worker by cloning an existing one.

    For from_worker_id, ALWAYS PICK A WORKER WITH A MINIMAL PROFILE: a hand-configured
    profile has been measured at +400MB of private bytes. 96 is 161B = minimal.
    min_worker_id is the caller's pool policy (MCP uses 98 and above).
    """
    if worker_id < min_worker_id:
        raise WorkerError(f"worker-{worker_id} is off limits "
                          f"({min_worker_id} and above)")
    workers_root = Path(workers_root)
    src = workers_root / f"worker-{from_worker_id}"
    dst = workers_root / f"worker-{worker_id}"
    if not src.exists():
        raise WorkerError(f"source worker not found: {src}")
    if dst.exists():
        raise WorkerError(f"worker-{worker_id} already exists: {dst}")
    # Do not copy while it is running (its exe/dll are held)
    busy = processes_under(src)
    if busy:
        raise WorkerError(f"worker-{from_worker_id} is running (PID {busy}). "
                          "Name a different, idle worker to clone from.")

    man = worker_manifest(from_worker_id, workers_root)
    base_game = Path(man["base_game_dir"])
    dst.mkdir(parents=True)
    # Resources is a junction. Following it through would copy 1.5GB, so exclude it
    # and re-create a junction to the same target instead.
    for entry in src.iterdir():
        if entry.name == "Resources":
            continue
        if entry.is_dir():
            shutil.copytree(entry, dst / entry.name, symlinks=True)
        else:
            shutil.copy2(entry, dst / entry.name)
    _junction(dst / "Resources", base_game / "Resources")

    # The exe gets its own name per ID. With a unique process name, cleanup is just
    # taskkill /IM, and there is no room to take out someone else's worker with it.
    (dst / f"GeometryDash-worker-{from_worker_id}.exe").replace(
        dst / f"GeometryDash-worker-{worker_id}.exe")

    # The source's session/data still holds measurement results, so empty it
    data = dst / "session" / "data"
    if data.exists():
        shutil.rmtree(data)
    data.mkdir(parents=True)

    # The save has a separate root per worker. Clone the minimal profile
    # (the window mode is persisted here too, so cloning keeps it windowed).
    save_dst = gd_save_root(worker_id)
    if save_dst.exists():
        shutil.rmtree(save_dst, ignore_errors=True)
    # `dirs_exist_ok` IS REQUIRED. Windows sometimes has not finished deleting the
    # directory even right after rmtree (a lingering handle turns it into a deferred
    # delete), and a plain copytree then dies with FileExistsError. In the move to C:
    # on 2026-08-21, 6 of 8 machines failed part-way here and left half-finished
    # workers with no worker.json.
    shutil.copytree(gd_save_root(from_worker_id), save_dst, dirs_exist_ok=True)

    # The appid is fixed at 480 (Valve's public test appid). Grabbing 322170 makes the
    # user's Steam copy of GD unable to launch.
    (dst / "steam_appid.txt").write_text("480", encoding="ascii", newline="")

    (dst / "worker.json").write_text(json.dumps({
        "worker_id": str(worker_id),
        "created_utc": datetime.now(timezone.utc).isoformat(),
        "base_game_dir": str(base_game),
        "executable": str(dst / f"GeometryDash-worker-{worker_id}.exe"),
        "data_root": str(data),
        "geode_root": str(dst / "geode"),
        "gd_save_root": str(save_dst),
        "app_id": 322170,
    }, indent=4), encoding="ascii")

    repair_save(worker_id)   # build a minimal windowed save if it is corrupt or absent
    size = sum(f.stat().st_size for f in dst.rglob("*")
               if f.is_file() and not f.is_symlink())
    print(f"worker-{worker_id} created ({size / 1024 / 1024:,.0f} MB, "
          f"from worker-{from_worker_id})")
    return dst


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(prog="gdtas.provision")
    ap.add_argument("--worker-id", type=int, required=True)
    ap.add_argument("--from-worker-id", type=int, default=96,
                    help="the worker to clone from; pick one with a minimal profile")
    ap.add_argument("--workers-root", default=str(WORKERS_ROOT))
    ap.add_argument("--min-worker-id", type=int, default=0,
                    help="which pool this belongs to; use 98 for an MCP worker")
    a = ap.parse_args(argv)
    create_worker(a.worker_id, a.from_worker_id, Path(a.workers_root),
                  a.min_worker_id)
    return 0


if __name__ == "__main__":
    sys.exit(main())
