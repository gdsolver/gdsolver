"""Paths to the repository, the build outputs and the machine-local trees.

Everything here is derived from the location of this file, so a clone works
without editing. Six environment variables override the machine-local parts:

    GDSOLVER_LAB       private tree: official-level dumps, gdref, fixups_log
                       archives, development notes            (default <repo>/../GD-lab)
    GDSOLVER_WORKERS   root of the isolated GD instances      (default <repo>/../GD-workers)
    GDSOLVER_GD_BASE   the frozen game build the workers run  (default <workers>/base-<gd.win>)
    GDSOLVER_GD_BUILD  its name, when mod.json's gd.win has more than one build (default gd.win)
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


def _machine_local(name: str, env: str) -> Path:
    """Locate a tree that sits next to the MAIN checkout, not this one.

    From a git worktree (<main>/.claude/worktrees/<x>) REPO.parent points
    inside the main checkout, so walk the parents for the first that has the
    tree. The plain-checkout case is parents[0], i.e. unchanged behaviour.
    """
    override = os.environ.get(env)
    if override:
        return Path(override)
    for p in REPO.parents:
        if (p / name).exists():
            return p / name
    return REPO.parent / name


# The private side: dumps of the official levels (objrects / triggers /
# objgroups / obb), the gdref reference traces, the append-only fixups_log
# archives, and the records of isolated runs. Never part of the public tree.
LAB = _machine_local("GD-lab", "GDSOLVER_LAB")
LEVEL_DATA = LAB / "data"
# Calibration rigs: the generated levels (.lvl / .units.json / .plan.txt) are
# public, the game's measurements of them (dump.csv, objrects_calib_*) are not.
RIGS = DATA / "rigs"
LAB_RIGS = LAB / "rigs"
SRC = REPO / "src"
SCRIPTS = REPO / "scripts"
DOCS = REPO / "docs"
# Geode's CMake writes build/<mod.id>.geode, so the package name follows the id.
# Read it rather than repeating it: the name was spelled out in four places and
# every one of them would have gone on deploying a file that no longer existed.
_MOD_JSON = json.loads((REPO / "mod.json").read_text(encoding="utf-8"))
MOD_ID = _MOD_JSON["id"]
BUILD_MOD = REPO / "build" / f"{MOD_ID}.geode"
# The build of the game the mod targets, and so the one every measurement here
# was taken on. The bindings and Geode's own load check follow it.
# GDSOLVER_GD_BUILD names the frozen build when one gd.win covers two executables (2.209 and its hotfix, say): the manifest of the base and every worker's files are then
# checked against that name, and the mod built for it says which it was built for (GEODE_COMP_GD_VERSION).
GD_VERSION = os.environ.get("GDSOLVER_GD_BUILD") or _MOD_JSON["gd"]["win"]
GEODE_VERSION = _MOD_JSON["geode"]
# Where a measurement session snapshots the .geode it is using, so that a
# rebuild of the mainline does not swap the binary under a running measurement.
MOD_CACHE = REPO / "mcp" / ".cache"

WORKERS_ROOT = _machine_local("GD-workers", "GDSOLVER_WORKERS")
# The frozen copy of that build which the workers are seeded from and checked
# against at every launch (gdtas/gdbase.py). One directory per build, so a
# mod.json naming a new build finds nothing until that build is frozen too.
GD_BASE = Path(os.environ.get("GDSOLVER_GD_BASE")
               or WORKERS_ROOT / f"base-{GD_VERSION}")
# The workers that run the build the mod targets: the game's current executable
# (GD 2.209's hotfix build, frozen as base-2.209b). A tool that picks a worker for
# itself starts from these and from nothing else -- a worker of another build (an
# older executable, another GD version) refuses the package or loads it with its
# hooks off.
#   DEFAULT_POOL    what a batch spreads its levels over (cold_regress and friends);
#                   a one-session run uses the first.
#   DEFAULT_WORKER  a tool that needs one worker takes the first of the pool, which is
#                   also the one a batch reaches first: name another whenever a batch
#                   may be running.
#   MCP_WORKER      the resident MCP session and the probes that share its policy
#                   (gdprobe, regen_groups): the second, so they sit beside a batch
#                   on DEFAULT_WORKER rather than under it.
#   PERSON_WORKER   the tools a person sits at (watch, play, screenshot): the last,
#                   which a batch reaches last.
# Two GD processes sharing one data root mix their result.txt, so a launch is refused
# while a GD already runs under the worker (gdtas.worker._wait_free): name a free one
# with --pool / --worker / --worker-id when a batch is running.
DEFAULT_POOL = (99, 100, 101)
DEFAULT_WORKER = DEFAULT_POOL[0]
MCP_WORKER = DEFAULT_POOL[1]
PERSON_WORKER = DEFAULT_POOL[-1]
# The MCP server refuses ids below this one.
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
