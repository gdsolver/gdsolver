"""Build a worker SEED straight from the GD install (for when no clone source is left).

    python -m gdtas.seed_worker --worker-id 96

`provision` clones an existing worker, so it is no use once they are all gone. This
one assembles the first machine out of Steam's GD install + the Geode SDK binaries +
a built MOD. After that, multiply with `provision --from-worker-id 96`.

Written on 2026-08-21, when D: (a USB SSD) died, ``D:\\GD-workers`` became entirely
unreadable, and not a single clone source was left.
"""

from __future__ import annotations

import argparse
import json
import shutil
import subprocess
import sys
from datetime import datetime, timezone
from pathlib import Path

from .paths import REPO, WORKERS_ROOT, gd_save_root
from .worker import CREATE_NO_WINDOW, WorkerError, repair_save

# Steam's default location. Can be overridden with --base-game.
DEFAULT_BASE_GAME = Path(
    r"C:\Program Files (x86)\Steam\steamapps\common\Geometry Dash")
# The Geode loader lives in the SDK's bin (keep the version in step with mod.json).
DEFAULT_SDK = Path.home() / "Documents" / "Geode"
LOADER_FILES = ("Geode.dll", "XInput1_4.dll", "GeodeUpdater.exe")


def _junction(link: Path, target: Path) -> None:
    r = subprocess.run(["cmd", "/c", "mklink", "/J", str(link), str(target)],
                       capture_output=True, text=True, errors="replace",
                       creationflags=CREATE_NO_WINDOW)
    if r.returncode != 0 or not link.exists():
        raise WorkerError(f"mklink /J failed: {r.stdout}{r.stderr}")


def seed_worker(worker_id: int, base_game: Path = DEFAULT_BASE_GAME,
                sdk: Path = DEFAULT_SDK, mod: Path | None = None,
                workers_root: Path = WORKERS_ROOT) -> Path:
    base_game, sdk = Path(base_game), Path(sdk)
    workers_root = Path(workers_root)
    if not (base_game / "GeometryDash.exe").exists():
        raise WorkerError(f"the base GD install is missing: {base_game}")
    dst = workers_root / f"worker-{worker_id}"
    if dst.exists():
        raise WorkerError(f"worker-{worker_id} already exists: {dst}")

    # Use the Geode version written in mod.json (the SDK's bin/<ver>).
    ver = json.loads((REPO / "mod.json").read_text(encoding="utf-8"))["geode"]
    loader_dir = sdk / "bin" / ver
    missing = [f for f in LOADER_FILES if not (loader_dir / f).exists()]
    if missing:
        raise WorkerError(f"the Geode loader is incomplete ({loader_dir}): {missing}")

    if mod is None:
        mod = REPO / "build" / "gdsolver.geode"
    mod = Path(mod)
    if not mod.exists():
        raise WorkerError(f"the mod is missing: {mod} (run cmake --build build first)")

    # 1. Clone the GD install (Resources excluded; it becomes a junction)
    dst.mkdir(parents=True)
    for entry in base_game.iterdir():
        if entry.name == "Resources":
            continue
        if entry.is_dir():
            shutil.copytree(entry, dst / entry.name, symlinks=True)
        else:
            shutil.copy2(entry, dst / entry.name)
    _junction(dst / "Resources", base_game / "Resources")

    # 2. The exe gets its own name per ID (so taskkill /IM cannot take out others)
    (dst / "GeometryDash.exe").replace(dst / f"GeometryDash-worker-{worker_id}.exe")

    # 3. Geode loader + MOD
    for f in LOADER_FILES:
        shutil.copy2(loader_dir / f, dst / f)
    mods = dst / "geode" / "mods"
    mods.mkdir(parents=True, exist_ok=True)
    shutil.copy2(mod, mods / mod.name)

    # 4. appid fixed at 480 (grabbing 322170 stops the user's Steam copy from starting)
    (dst / "steam_appid.txt").write_text("480", encoding="ascii", newline="")

    data = dst / "session" / "data"
    data.mkdir(parents=True, exist_ok=True)
    save_dst = gd_save_root(worker_id)
    save_dst.mkdir(parents=True, exist_ok=True)

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

    repair_save(worker_id)
    size = sum(f.stat().st_size for f in dst.rglob("*")
               if f.is_file() and not f.is_symlink())
    print(f"worker-{worker_id} seeded ({size / 1024 / 1024:,.0f} MB, "
          f"geode {ver}, mod {mod.name})")
    return dst


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(prog="gdtas.seed_worker")
    ap.add_argument("--worker-id", type=int, required=True)
    ap.add_argument("--base-game", default=str(DEFAULT_BASE_GAME))
    ap.add_argument("--sdk", default=str(DEFAULT_SDK))
    ap.add_argument("--mod", default="")
    ap.add_argument("--workers-root", default=str(WORKERS_ROOT))
    a = ap.parse_args(argv)
    seed_worker(a.worker_id, Path(a.base_game), Path(a.sdk),
                Path(a.mod) if a.mod else None, Path(a.workers_root))
    return 0


if __name__ == "__main__":
    sys.exit(main())
