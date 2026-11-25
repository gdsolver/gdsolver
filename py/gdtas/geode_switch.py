"""Moving the workers to another Geode loader, with a way back.

    python -m gdtas.geode_switch status
    python -m gdtas.geode_switch backup  --worker-id 101 [--backup-root DIR]
    python -m gdtas.geode_switch upgrade --worker-id 101 --sdk DIR --resources resources.zip
    python -m gdtas.geode_switch restore --worker-id 101 [--backup-root DIR]
    python -m gdtas.geode_switch verify  --worker-id 101
    python -m gdtas.geode_switch no-autoupdate --worker-id 101
    python -m gdtas.geode_switch park-update   --worker-id 101

A worker's Geode is three files at its root (Geode.dll, GeodeUpdater.exe, XInput1_4.dll), the loader's own resources
(geode/resources/geode.loader) and whatever the loader keeps in geode/ (mods, config, the unpacked mods). The loader's
version is what geode_pin.json names; every launch checks it (gdbase.check_geode).

`backup` copies those into BACKUP_ROOT/worker-NN/ with a manifest of SHA-256s and refuses to overwrite one. `upgrade`
needs a backup, an idle worker, the new loader's files (<sdk>/bin/<version>/) and its resources.zip; it replaces the three
files and the loader's resources, and empties what the loader unpacked itself (unzipped, temp) and the mod files in
geode/mods (the harness installs the mod it runs on every launch). `restore` puts the backup back. Nothing here starts a
game: the first launch after an upgrade is the person's (the lv1-3 check of the procedure).

`no-autoupdate` writes "auto-check-updates": false into the worker's own Geode settings (its save directory,
geode/mods/geode.loader/settings.json): a loader that checks for updates stages a new one in geode/update, which the
harness refuses at the next launch (and which the loader would apply, restarting the game without our arguments).
`upgrade` does it. `park-update` moves a staged geode/update into BACKUP_ROOT/staged_updates/ (a move, never a delete).

The worker's own save and its Resources junction are not touched. `verify` also reads the worker's window size index:
the cold baseline is measured at 25 (1706x960), and a worker left on another index refuses the run.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import shutil
import sys
import zipfile
from datetime import datetime, timezone
from pathlib import Path

from .gdbase import GEODE_FILES, geode_identity, geode_pin, geode_problems
from .paths import LAB, WORKERS_ROOT, gd_save_root, worker_dir
from .worker import WorkerError, processes_under

DEFAULT_BACKUP_ROOT = LAB / "arms" / "geode5_backup"
LOADER_RES = Path("geode") / "resources" / "geode.loader"
KEEP_DIRS = ("mods", "config")          # backed up as they are
EMPTIED_DIRS = ("unzipped", "temp")     # the loader's own unpacking: rebuilt at the next launch
BASELINE_RESOLUTION_INDEX = 25


def _sha256(p: Path) -> str:
    h = hashlib.sha256()
    with open(p, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def _tree_hashes(root: Path) -> dict[str, str]:
    out = {}
    if root.is_dir():
        for p in sorted(root.rglob("*")):
            if p.is_file():
                out[p.relative_to(root).as_posix()] = _sha256(p)
    return out


def _require_idle(root: Path) -> None:
    busy = processes_under(root)
    if busy:
        raise WorkerError(f"{root.name} is running (PID {busy}): stop it first")


def backup(worker_id: int, backup_root: Path = DEFAULT_BACKUP_ROOT, workers_root: Path = WORKERS_ROOT) -> Path:
    root = worker_dir(worker_id, workers_root)
    if not root.is_dir():
        raise WorkerError(f"no such worker: {root}")
    _require_idle(root)
    dst = Path(backup_root) / f"worker-{worker_id}"
    if dst.exists():
        raise WorkerError(f"a backup already exists: {dst} (a backup is never overwritten)")
    dst.mkdir(parents=True)
    manifest = {"worker": worker_id, "created_utc": datetime.now(timezone.utc).isoformat(),
                "identity": geode_identity(root), "files": {}, "trees": {}}
    for name in GEODE_FILES:
        shutil.copy2(root / name, dst / name)
        manifest["files"][name] = _sha256(root / name)
    res = root / LOADER_RES
    if res.is_dir():
        shutil.copytree(res, dst / "geode.loader")
        manifest["trees"]["geode.loader"] = _tree_hashes(dst / "geode.loader")
    for d in KEEP_DIRS:
        src = root / "geode" / d
        if src.is_dir():
            shutil.copytree(src, dst / d)
            manifest["trees"][d] = _tree_hashes(dst / d)
    (dst / "manifest.json").write_text(json.dumps(manifest, indent=1), encoding="utf-8")
    return dst


def _backup_ok(dst: Path) -> dict:
    mf = dst / "manifest.json"
    if not mf.is_file():
        raise WorkerError(f"no backup at {dst}: run `backup` first")
    m = json.loads(mf.read_text(encoding="utf-8"))
    for name, h in m["files"].items():
        if not (dst / name).is_file() or _sha256(dst / name) != h:
            raise WorkerError(f"the backup's {name} does not match its manifest")
    for tree, files in m["trees"].items():
        if _tree_hashes(dst / tree) != files:
            raise WorkerError(f"the backup's {tree} does not match its manifest")
    return m


def _clear(path: Path) -> None:
    if path.is_dir():
        shutil.rmtree(path)
    path.mkdir(parents=True, exist_ok=True)


def upgrade(worker_id: int, sdk: Path, resources_zip: Path, version_dir: str | None = None,
            backup_root: Path = DEFAULT_BACKUP_ROOT, workers_root: Path = WORKERS_ROOT) -> None:
    root = worker_dir(worker_id, workers_root)
    _require_idle(root)
    _backup_ok(Path(backup_root) / f"worker-{worker_id}")
    bins = Path(sdk) / "bin"
    vdir = bins / version_dir if version_dir else None
    if vdir is None:
        cands = sorted(p for p in bins.iterdir() if p.is_dir()) if bins.is_dir() else []
        if len(cands) != 1:
            raise WorkerError(f"{bins} holds {len(cands)} loader directories: name one with --version-dir")
        vdir = cands[0]
    for name in GEODE_FILES:
        if not (vdir / name).is_file():
            raise WorkerError(f"missing {vdir / name}")
    if not zipfile.is_zipfile(resources_zip):
        raise WorkerError(f"not a zip: {resources_zip}")
    for name in GEODE_FILES:
        shutil.copy2(vdir / name, root / name)
    res = root / LOADER_RES
    _clear(res)
    with zipfile.ZipFile(resources_zip) as z:
        z.extractall(res)
    for d in EMPTIED_DIRS:
        _clear(root / "geode" / d)
    mods = root / "geode" / "mods"
    if mods.is_dir():
        for p in mods.glob("*.geode"):
            p.unlink()
    park_update(worker_id, backup_root, workers_root)
    no_autoupdate(worker_id)


def no_autoupdate(worker_id: int) -> Path:
    """Switch the worker's loader's own update check off; returns the settings file."""
    p = Path(gd_save_root(worker_id)) / "geode" / "mods" / "geode.loader" / "settings.json"
    p.parent.mkdir(parents=True, exist_ok=True)
    try:
        s = json.loads(p.read_text(encoding="utf-8-sig")) if p.is_file() else {}
    except ValueError:
        raise WorkerError(f"{p} is not JSON: not touched")
    s["auto-check-updates"] = False
    p.write_text(json.dumps(s, indent=4) + "\n", encoding="utf-8")
    return p


def park_update(worker_id: int, backup_root: Path = DEFAULT_BACKUP_ROOT, workers_root: Path = WORKERS_ROOT) -> Path | None:
    """Move a staged loader update out of the worker (a move: nothing is deleted)."""
    root = worker_dir(worker_id, workers_root)
    _require_idle(root)
    staged = root / "geode" / "update"
    if not staged.exists():
        return None
    dst_dir = Path(backup_root) / "staged_updates"
    dst_dir.mkdir(parents=True, exist_ok=True)
    dst = dst_dir / f"worker-{worker_id}_update_{datetime.now().strftime('%m%d_%H%M%S')}"
    shutil.move(str(staged), str(dst))
    return dst


def restore(worker_id: int, backup_root: Path = DEFAULT_BACKUP_ROOT, workers_root: Path = WORKERS_ROOT) -> None:
    root = worker_dir(worker_id, workers_root)
    _require_idle(root)
    dst = Path(backup_root) / f"worker-{worker_id}"
    _backup_ok(dst)
    for name in GEODE_FILES:
        shutil.copy2(dst / name, root / name)
    for tree, target in (("geode.loader", root / LOADER_RES), ("mods", root / "geode" / "mods"),
                         ("config", root / "geode" / "config")):
        if (dst / tree).is_dir():
            if target.exists():
                shutil.rmtree(target)
            shutil.copytree(dst / tree, target)
    for d in EMPTIED_DIRS:
        _clear(root / "geode" / d)
    park_update(worker_id, backup_root, workers_root)


def verify(worker_id: int, workers_root: Path = WORKERS_ROOT) -> list[str]:
    """Problems with a worker after a switch; empty when it is ready for the lv1-3 check."""
    from . import gdsave
    root = worker_dir(worker_id, workers_root)
    bad = list(geode_problems(root, geode_pin()))
    idx = gdsave.resolution_index(worker_id)
    if idx != BASELINE_RESOLUTION_INDEX:
        bad.append(f"window size index is {idx}, the baseline is measured at {BASELINE_RESOLUTION_INDEX}: "
                   f"gdsave.set_resolution_index({worker_id}, {BASELINE_RESOLUTION_INDEX})")
    if not (root / "Resources").exists():
        bad.append("Resources junction is missing")
    return bad


def status(workers_root: Path = WORKERS_ROOT, backup_root: Path = DEFAULT_BACKUP_ROOT) -> int:
    pin = geode_pin()
    print(f"pin: {pin['version'] if pin else 'none'}")
    rc = 0
    for root in sorted(Path(workers_root).glob("worker-*"), key=lambda p: int(p.name.split("-")[1])):
        wid = int(root.name.split("-")[1])
        ident = geode_identity(root)
        bad = geode_problems(root, pin)
        have_backup = (Path(backup_root) / root.name / "manifest.json").is_file()
        run = processes_under(root)
        print(f"{root.name}: loader {ident['version']} dll {str(ident['dll_sha256'])[:8]}  "
              f"{'pinned' if not bad else 'NOT PINNED'}  backup {'yes' if have_backup else 'no'}"
              f"{'  RUNNING ' + str(run) if run else ''}")
        rc |= 1 if bad else 0
    return rc


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(prog="gdtas.geode_switch", description=__doc__.split("\n\n")[0])
    sub = ap.add_subparsers(dest="cmd", required=True)
    for name in ("status", "backup", "upgrade", "restore", "verify", "no-autoupdate", "park-update"):
        p = sub.add_parser(name)
        p.add_argument("--workers-root", default=str(WORKERS_ROOT))
        p.add_argument("--backup-root", default=str(DEFAULT_BACKUP_ROOT))
        if name != "status":
            p.add_argument("--worker-id", type=int, required=True)
        if name == "upgrade":
            p.add_argument("--sdk", required=True, help="the Geode SDK directory (its bin/<version>/ holds the loader)")
            p.add_argument("--resources", required=True, help="the release's resources.zip")
            p.add_argument("--version-dir", default=None)
    a = ap.parse_args(argv)
    wr, br = Path(a.workers_root), Path(a.backup_root)
    if a.cmd == "status":
        return status(wr, br)
    if a.cmd == "backup":
        print(backup(a.worker_id, br, wr))
    elif a.cmd == "upgrade":
        upgrade(a.worker_id, Path(a.sdk), Path(a.resources), a.version_dir, br, wr)
        print(f"worker-{a.worker_id} upgraded; next: verify, then the lv1-3 check")
    elif a.cmd == "restore":
        restore(a.worker_id, br, wr)
        print(f"worker-{a.worker_id} restored from {br / ('worker-' + str(a.worker_id))}")
    elif a.cmd == "no-autoupdate":
        print(no_autoupdate(a.worker_id))
    elif a.cmd == "park-update":
        print(park_update(a.worker_id, br, wr) or "nothing staged")
    elif a.cmd == "verify":
        bad = verify(a.worker_id, wr)
        for b in bad:
            print("PROBLEM:", b)
        print("ok" if not bad else f"{len(bad)} problem(s)")
        return 1 if bad else 0
    return 0


if __name__ == "__main__":
    sys.exit(main())
