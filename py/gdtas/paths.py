"""Paths to the repository, the build outputs and the machine-local trees.

Everything here is derived from the location of this file, so a clone works
without editing. Four environment variables override the machine-local parts:

    GDSOLVER_LAB       private tree: official-level dumps, gdref, fixups_log
                       archives, development notes            (default <repo>/../GD-lab)
    GDSOLVER_WORKERS   root of the isolated GD instances      (default <repo>/../GD-workers)
    GDSOLVER_LEVELDP   the solver executable to drive         (default <repo>/build/dp/...)
    GD_DIR             the Geometry Dash installation         (used by py/dev.py)
"""

from __future__ import annotations

import json
import os
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent.parent
# Results (solution plans) and the per-run working files. Only the solutions
# are tracked (see .gitignore's allow-list).
DATA = REPO / "data"
# The private side: dumps of the official levels (objrects / triggers /
# objgroups / obb), the gdref reference traces, the append-only fixups_log
# archives, and the records of isolated runs. Never part of the public tree.
LAB = Path(os.environ.get("GDSOLVER_LAB") or REPO.parent / "GD-lab")
LEVEL_DATA = LAB / "data"
# Calibration rigs: the generated levels (.lvl / .units.json / .plan.txt) are
# public, the game's measurements of them (dump.csv, objrects_calib_*) are not.
RIGS = DATA / "rigs"
LAB_RIGS = LAB / "rigs"
SRC = REPO / "src"
SCRIPTS = REPO / "scripts"
DOCS = REPO / "docs"
BUILD_MOD = REPO / "build" / "gdsolver.geode"
# Where a measurement session snapshots the .geode it is using, so that a
# rebuild of the mainline does not swap the binary under a running measurement.
MOD_CACHE = REPO / "mcp" / ".cache"

WORKERS_ROOT = Path(os.environ.get("GDSOLVER_WORKERS") or REPO.parent / "GD-workers")
# The range the mainline batch (cold_regress and friends) claims as its pool.
# Two GD processes sharing one data root mix their result.txt, so the resident
# side (the MCP server) has to use ids outside this range.
MAIN_POOL = tuple(range(90, 98))
MCP_MIN_WORKER_ID = 98

# The reachability-DP solver. Built from dp/ in this repository; the historical
# standalone build (GD-approx-physics) can still be pointed at with the
# environment variable, which is how frozen revisions are A/B-compared.
_DEFAULT_LEVELDP = REPO / "build" / "dp" / "RelWithDebInfo" / "leveldp.exe"
if not _DEFAULT_LEVELDP.exists():
    for _cand in (REPO / "build-dp" / "dp" / "Release" / "leveldp.exe",
                  REPO / "build" / "dp" / "Release" / "leveldp.exe"):
        if _cand.exists():
            _DEFAULT_LEVELDP = _cand
            break
LEVELDP_EXE = Path(os.environ.get("GDSOLVER_LEVELDP") or _DEFAULT_LEVELDP)
LEVELDP_REPO = LEVELDP_EXE.parent

VERIFY_RESULT = DATA / "verify_solutions_result.txt"


def worker_dir(worker_id: int, workers_root: Path | str = WORKERS_ROOT) -> Path:
    return Path(workers_root) / f"worker-{worker_id}"


def worker_manifest(worker_id: int, workers_root: Path | str = WORKERS_ROOT) -> dict:
    """Read worker.json. Some were written with a BOM, hence utf-8-sig."""
    p = worker_dir(worker_id, workers_root) / "worker.json"
    if not p.exists():
        raise FileNotFoundError(
            f"{p} does not exist. Run `python -m gdtas.provision --worker-id {worker_id}` first")
    return json.loads(p.read_text(encoding="utf-8-sig"))


def gd_save_root(worker_id: int) -> Path:
    """Each worker gets its own save root (window mode persists in there too)."""
    return Path(os.environ["LOCALAPPDATA"]) / f"GeometryDash-worker-{worker_id}"


def solution_file(level: int, tag: str = "dp") -> Path:
    return DATA / f"solution_lv{level}_{tag}.txt"
