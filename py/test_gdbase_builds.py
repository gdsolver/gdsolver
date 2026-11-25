"""Which base a worker is checked against and which packages it accepts (gdtas.gdbase): its own base, from worker.json, and a package whose gd.win names
that game -- a checkout whose mod.json says 2.209 must still run a 2.2081 worker with a 2.2081 package, and refuse the mismatches.

    python py/test_gdbase_builds.py        (no game, no worker: temp directories)
"""
import json
import os
import sys
import tempfile
import zipfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
os.environ.pop("GDSOLVER_GD_BASE", None)

from gdtas import gdbase  # noqa: E402


def package(dirp: Path, win: str) -> Path:
    p = dirp / f"pkg_{win}.geode"
    with zipfile.ZipFile(p, "w") as z:
        z.writestr("mod.json", json.dumps({"id": "x", "gd": {"win": win, "android": "2.2081"}}))
    return p


def worker(root: Path, base: Path) -> None:
    root.mkdir(parents=True)
    (root / "worker.json").write_text(json.dumps({"base_game_dir": str(base / "game")}), encoding="ascii")


with tempfile.TemporaryDirectory() as t:
    t = Path(t)
    for ver in ("2.2081", "2.209", "2.209b"):
        b = t / f"base-{ver}"
        b.mkdir()
        (b / gdbase.MANIFEST).write_text(json.dumps({"gd_version": ver, "files": {}}), encoding="ascii")
    w_old, w_new, w_hot = t / "worker-1", t / "worker-2", t / "worker-3"
    worker(w_old, t / "base-2.2081")
    worker(w_new, t / "base-2.209")
    worker(w_hot, t / "base-2.209b")

    # each worker's own base
    assert gdbase.worker_base(w_old).name == "base-2.2081"
    assert gdbase.worker_base(w_new).name == "base-2.209"
    assert gdbase.worker_base(w_hot).name == "base-2.209b"
    # a worker with no worker.json, or one naming a base that has no manifest, gets the default
    (t / "worker-7").mkdir()
    assert gdbase.worker_base(t / "worker-7") == gdbase.GD_BASE
    worker(t / "worker-8", t / "base-nowhere")
    assert gdbase.worker_base(t / "worker-8") == gdbase.GD_BASE

    # which package runs on which game
    assert gdbase.same_build("2.2081", "2.2081")
    assert gdbase.same_build("2.209", "2.209")
    assert gdbase.same_build("2.209b", "2.209")          # the hotfix is frozen as 2.209b, the package says 2.209
    assert gdbase.same_build("2.209b", "2.2091")         # Geode 6 names the hotfix build 2.2091 in gd.win
    assert not gdbase.same_build("2.2081", "2.2091")
    assert not gdbase.same_build("2.209", "2.2091")      # the first 2.209 executable is not the 2.2091 one
    assert not gdbase.same_build("2.2081", "2.209")
    assert not gdbase.same_build("2.209", "2.2081")
    assert not gdbase.same_build("2.2091", "2.209")      # a digit after the version is another version, not a letter
    assert not gdbase.same_build("2.209b", "2.2081")

    # the problems the version check reports (the file checks list nothing: empty manifests)
    def version_problem(root: Path, win: str):
        base = gdbase.worker_base(root)
        bad = gdbase._worker_problems(root, root / "x.exe", base, gdbase.mod_gd_version(package(t, win)))
        return [b for b in bad if b.startswith("the base is GD")]

    assert not version_problem(w_old, "2.2081")
    assert version_problem(w_old, "2.209")
    assert not version_problem(w_new, "2.209")
    assert version_problem(w_new, "2.2081")
    assert not version_problem(w_hot, "2.209")
    assert version_problem(w_hot, "2.2081")

    # a package that cannot be read says None, and a missing gd.win does too
    assert gdbase.mod_gd_version(t / "nothing.geode") is None
    junk = t / "junk.geode"
    junk.write_bytes(b"not a zip")
    assert gdbase.mod_gd_version(junk) is None
print("ok")
