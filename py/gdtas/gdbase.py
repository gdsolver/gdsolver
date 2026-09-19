"""The frozen build of Geometry Dash the workers run.

    python -m gdtas.gdbase freeze [--from DIR]    copy an install into GD_BASE and hash it
    python -m gdtas.gdbase verify [--full]        the base against its manifest
    python -m gdtas.gdbase status                 every worker against the base
    python -m gdtas.gdbase repoint [--worker-id N]  move workers' Resources onto the base

Every measurement here -- the model's constants, the rigs, the baseline
iteration counts -- was taken on one build of the game, so the workers have to
go on running that build when Steam moves to the next one. A worker's
executable and DLLs are copies, but its Resources used to be a junction to the
Steam install itself: an update would have replaced the resources under every
worker while their executables stayed behind, and nothing would have said so.
Geode checks the executable's version and nothing else.

The base is a copy of the install that Steam does not manage, one directory per
build, named after mod.json's `gd.win` (paths.GD_BASE):

    base-<ver>/game/           the install, without the Geode loader or its data
    base-<ver>/manifest.json   size and SHA-256 of every file in game/ and geode-bin/
    base-<ver>/geode-bin/      the loader binaries of the Geode version in mod.json

Every launch checks (check_worker) that the worker's executable and DLLs are the
base's and that its Resources is the base's. A mod.json that names a new build
fails that check until the new build has been frozen too, which is the point:
nothing runs a new build by accident. The old base stays, so the two can be
compared side by side.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import shutil
import sys
from datetime import datetime, timezone
from pathlib import Path

from .paths import GD_BASE, GD_VERSION, GEODE_VERSION, WORKERS_ROOT, worker_dir

STEAM_INSTALL = Path(r"C:\Program Files (x86)\Steam\steamapps\common\Geometry Dash")
GEODE_SDK = Path(os.environ.get("GEODE_SDK") or Path.home() / "Documents" / "Geode")
# What Geode adds to an install. The loader comes from the SDK's bin/<ver> when a
# worker is seeded, and geode/ holds the owner's own mods, settings and logs.
NOT_THE_GAME = {"geode", "Geode.dll", "Geode.pdb", "GeodeUpdater.exe", "XInput1_4.dll"}
MANIFEST = "manifest.json"
_CHUNK = 1 << 20


class GDBaseMismatch(RuntimeError):
    """The worker is not running the frozen build. Not a WorkerError on purpose:
    retrying a launch cannot fix it, and a batch that treats it as a failed level
    would report the level, not the game."""


def _sha256(p: Path) -> str:
    h = hashlib.sha256()
    with open(p, "rb") as f:
        while b := f.read(_CHUNK):
            h.update(b)
    return h.hexdigest()


def _copy_hashed(src: Path, dst: Path) -> str:
    """Copy one file and return the SHA-256 of what was read from the source."""
    h = hashlib.sha256()
    with open(src, "rb") as fi, open(dst, "wb") as fo:
        while b := fi.read(_CHUNK):
            h.update(b)
            fo.write(b)
    shutil.copystat(src, dst)
    return h.hexdigest()


def _copy_tree(src: Path, dst: Path, files: dict, prefix: str, skip_top=()) -> None:
    for d, dirs, names in os.walk(src):
        rel_d = Path(d).relative_to(src)
        if rel_d == Path("."):
            dirs[:] = [x for x in dirs if x not in skip_top]
            names = [x for x in names if x not in skip_top]
        (dst / rel_d).mkdir(parents=True, exist_ok=True)
        for n in names:
            rel = (rel_d / n).as_posix()
            files[prefix + rel] = {
                "size": (Path(d) / n).stat().st_size,
                "sha256": _copy_hashed(Path(d) / n, dst / rel_d / n)}


def _steam_buildid(install: Path) -> str | None:
    acf = install.parent.parent / "appmanifest_322170.acf"
    try:
        m = re.search(r'"buildid"\s+"(\d+)"', acf.read_text(errors="replace"))
    except OSError:
        return None
    return m.group(1) if m else None


def _sdk_commit() -> str | None:
    head = GEODE_SDK / ".git" / "HEAD"
    try:
        ref = head.read_text().strip()
        if ref.startswith("ref: "):
            return (GEODE_SDK / ".git" / ref[5:]).read_text().strip()
        return ref
    except OSError:
        return None


def freeze(src: Path = STEAM_INSTALL, base: Path = GD_BASE, gd_version: str = GD_VERSION,
           geode_version: str = GEODE_VERSION) -> Path:
    """Copy an install into `base`, hash every file, then re-read the copy.

    The versions are what the install IS, which is mod.json's only while mod.json
    still describes it: freezing a new build before the mod has moved to it takes
    both, and the base directory is named after the build given.

    The copy is assembled under <base>.partial and renamed at the end, so a base
    either has a manifest that describes all of it or does not exist."""
    src, base = Path(src), Path(base)
    if base.exists():
        raise SystemExit(f"{base} already exists; a base is never refrozen in place")
    if not (src / "GeometryDash.exe").is_file():
        raise SystemExit(f"no GeometryDash.exe in {src}")
    loader = GEODE_SDK / "bin" / geode_version
    if not (loader / "Geode.dll").is_file():
        raise SystemExit(f"no Geode {geode_version} loader in {loader}")
    tmp = base.with_name(base.name + ".partial")
    if tmp.exists():
        shutil.rmtree(tmp)
    files: dict[str, dict] = {}
    print(f"copying {src} -> {tmp / 'game'}")
    _copy_tree(src, tmp / "game", files, "game/", skip_top=NOT_THE_GAME)
    print(f"copying {loader} -> {tmp / 'geode-bin'}")
    _copy_tree(loader, tmp / "geode-bin", files, "geode-bin/")
    man = {
        "gd_version": gd_version,
        "frozen_utc": datetime.now(timezone.utc).isoformat(timespec="seconds"),
        "source": str(src),
        "steam_buildid": _steam_buildid(src),
        # The SDK checkout's commit is only meaningful when it is the version named.
        "geode": {"version": geode_version,
                  "sdk_commit": _sdk_commit() if geode_version == GEODE_VERSION else None},
        "left_out": sorted(NOT_THE_GAME),
        "files": files,
    }
    (tmp / MANIFEST).write_text(json.dumps(man, indent=1), encoding="ascii")
    tmp.rename(base)
    bad = verify(base, full=True)
    for b in bad:
        print("  " + b)
    if bad:
        raise SystemExit(f"{base}: the copy does not read back as what was hashed")
    total = sum(f["size"] for f in files.values())
    print(f"frozen: {base} ({len(files)} files, {total / 1e6:,.0f} MB, "
          f"exe {files['game/GeometryDash.exe']['sha256'][:12]}, "
          f"steam build {man['steam_buildid']})")
    return base


def manifest(base: Path = GD_BASE) -> dict:
    p = Path(base) / MANIFEST
    if not p.is_file():
        raise GDBaseMismatch(
            f"no frozen GD {GD_VERSION} at {base}. Freeze the install first:\n"
            f"    python -m gdtas.gdbase freeze --from <the {GD_VERSION} install>")
    return json.loads(p.read_text(encoding="ascii"))


def verify(base: Path = GD_BASE, full: bool = False) -> list[str]:
    """Every file the manifest lists, at its size (and hash with `full`), and
    nothing it does not list."""
    base = Path(base)
    files = manifest(base)["files"]
    bad = []
    for rel, f in files.items():
        p = base / rel
        if not p.is_file():
            bad.append(f"missing {rel}")
        elif p.stat().st_size != f["size"]:
            bad.append(f"size {rel}: {p.stat().st_size} != {f['size']}")
        elif full and _sha256(p) != f["sha256"]:
            bad.append(f"sha256 {rel}")
    for top in ("game", "geode-bin"):
        for p in (base / top).rglob("*"):
            if p.is_file() and p.relative_to(base).as_posix() not in files:
                bad.append(f"not in the manifest: {p.relative_to(base).as_posix()}")
    return bad


_hash_cache: dict[tuple, str] = {}


def _sha256_cached(p: Path) -> str:
    st = p.stat()
    key = (str(p), st.st_size, st.st_mtime_ns)
    if key not in _hash_cache:
        _hash_cache[key] = _sha256(p)
    return _hash_cache[key]


def _worker_problems(root: Path, exe: Path, base: Path) -> list[str]:
    man = manifest(base)
    files = man["files"]
    bad = []
    if man["gd_version"] != GD_VERSION:
        bad.append(f"the base is GD {man['gd_version']}, mod.json targets {GD_VERSION}")
    for rel, f in files.items():
        if not rel.startswith("game/") or "/" in rel[5:]:
            continue
        name = rel[5:]
        p = Path(exe) if name == "GeometryDash.exe" else Path(root) / name
        if not p.is_file():
            bad.append(f"missing {p.name}")
        elif _sha256_cached(p) != f["sha256"]:
            bad.append(f"{p.name} is not the base's (GD {man['gd_version']})")
    res = Path(root) / "Resources"
    want = os.path.realpath(Path(base) / "game" / "Resources")
    have = os.path.realpath(res) if res.exists() else None
    if have is None:
        bad.append("no Resources")
    elif os.path.normcase(have) != os.path.normcase(want):
        bad.append(f"Resources is {have}, not the base's {want}")
    return bad


def check_worker(root: Path, exe: Path, base: Path = GD_BASE) -> None:
    """Refuse to launch a worker that is not running the frozen build."""
    bad = _worker_problems(root, exe, base)
    if bad:
        raise GDBaseMismatch(
            f"{Path(root).name} is not running the frozen GD {GD_VERSION} ({base}): "
            + "; ".join(bad)
            + "\n    (`python -m gdtas.gdbase status` for all workers, "
              "`repoint` for a stale Resources)")


def _workers(workers_root: Path) -> list[Path]:
    return sorted((d for d in Path(workers_root).glob("worker-*") if d.is_dir()),
                  key=lambda d: int(d.name.split("-")[1]) if d.name.split("-")[1].isdigit()
                  else 1 << 30)


def _worker_exe(root: Path) -> Path:
    return root / f"GeometryDash-{root.name}.exe"


def status(base: Path = GD_BASE, workers_root: Path = WORKERS_ROOT) -> int:
    man = manifest(base)
    print(f"base {base}: GD {man['gd_version']}, steam build {man['steam_buildid']}, "
          f"frozen {man['frozen_utc']}")
    n_bad = 0
    for root in _workers(workers_root):
        bad = _worker_problems(root, _worker_exe(root), base)
        n_bad += bool(bad)
        print(f"  {root.name:<10} {'ok' if not bad else '; '.join(bad)}")
    return 1 if n_bad else 0


def repoint(worker_id: int, base: Path = GD_BASE, workers_root: Path = WORKERS_ROOT) -> None:
    """Move one worker's Resources junction onto the base.

    Only a worker whose executable and DLLs already are the base's, and only
    while it is not running: this changes where its resources come from, not
    which build it is."""
    from .provision import _junction
    from .worker import processes_under

    root = worker_dir(worker_id, workers_root)
    busy = processes_under(root)
    if busy:
        raise SystemExit(f"{root.name} is running (PID {busy}); not touching it")
    bad = [b for b in _worker_problems(root, _worker_exe(root), base)
           if not b.startswith("Resources is ")]
    if bad:
        raise SystemExit(f"{root.name} is not the base's build: {'; '.join(bad)}")
    res = root / "Resources"
    target = Path(base) / "game" / "Resources"
    if res.exists() and os.path.normcase(os.path.realpath(res)) == \
            os.path.normcase(os.path.realpath(target)):
        print(f"{root.name}: already on the base")
        return
    if not res.is_junction():
        raise SystemExit(f"{res} is not a junction; not removing it")
    old = os.path.realpath(res)
    os.rmdir(res)   # removes the junction itself, never what it points at
    _junction(res, target)
    mpath = root / "worker.json"
    man = json.loads(mpath.read_text(encoding="utf-8-sig"))
    man["base_game_dir"] = str(Path(base) / "game")
    mpath.write_text(json.dumps(man, indent=4), encoding="ascii")
    print(f"{root.name}: Resources {old} -> {target}")


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(prog="gdtas.gdbase")
    sub = ap.add_subparsers(dest="cmd", required=True)
    f = sub.add_parser("freeze")
    f.add_argument("--from", dest="src", default=str(STEAM_INSTALL))
    f.add_argument("--gd-version", default=GD_VERSION,
                   help="the build the install is (default: mod.json's gd.win); "
                        "the base is then <workers>/base-<this>")
    f.add_argument("--geode-version", default=GEODE_VERSION,
                   help="the Geode loader to keep beside it (default: mod.json's)")
    v = sub.add_parser("verify")
    v.add_argument("--full", action="store_true", help="hash every file, not just sizes")
    s = sub.add_parser("status")
    r = sub.add_parser("repoint")
    r.add_argument("--worker-id", type=int, action="append",
                   help="repeatable; every worker when left out")
    for p in (f, v, s, r):
        p.add_argument("--base", default=None)
    a = ap.parse_args(argv)
    if a.cmd == "freeze":
        base = a.base or (GD_BASE if a.gd_version == GD_VERSION
                          else WORKERS_ROOT / f"base-{a.gd_version}")
        freeze(Path(a.src), Path(base), a.gd_version, a.geode_version)
        return 0
    a.base = a.base or str(GD_BASE)
    if a.cmd == "verify":
        bad = verify(Path(a.base), a.full)
        for b in bad:
            print(b)
        print(f"{a.base}: {'OK' if not bad else f'{len(bad)} problem(s)'}"
              f" ({'sizes and hashes' if a.full else 'sizes'})")
        return 1 if bad else 0
    if a.cmd == "status":
        return status(Path(a.base))
    ids = a.worker_id or [int(d.name.split("-")[1]) for d in _workers(WORKERS_ROOT)
                          if d.name.split("-")[1].isdigit()]
    for i in ids:
        repoint(i, Path(a.base))
    return 0


if __name__ == "__main__":
    sys.exit(main())
