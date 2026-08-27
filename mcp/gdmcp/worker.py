"""The GD worker as MCP sees it. The real thing is gdtas.worker (the shared library).

All that is left here is the MCP-specific policy:
  - Workers 98 and above only. 90-97 are the main line's Pool, so do not touch them
    (two GDs on the same data root mix up result.txt).
  - Writes always go to the worker's data_root. The repository's own data/ is only
    ever read from.

Lifecycle management, the serve protocol and the result.txt contract are written on
the gdtas side and nowhere else. Do not go back to a double implementation.
"""

from __future__ import annotations

import importlib
import sys
from pathlib import Path

# gd_reload only touches gdmcp.worker. The real code is on the gdtas side, so reload it
# here as well to keep the "fix without restarting Claude Code" path intact (list them
# in dependency order).
for _m in ("gdtas.paths", "gdtas.results", "gdtas.plan", "gdtas.worker"):
    if _m in sys.modules:
        importlib.reload(sys.modules[_m])

from gdtas.paths import BUILD_MOD, MCP_MIN_WORKER_ID, MOD_CACHE, WORKERS_ROOT
from gdtas.worker import BASE_CFG, RunResult, WorkerError
from gdtas.worker import Worker as _Worker

MIN_WORKER_ID = MCP_MIN_WORKER_ID
CACHE_DIR = MOD_CACHE

__all__ = ["Worker", "WorkerError", "RunResult", "BASE_CFG", "BUILD_MOD",
           "WORKERS_ROOT", "MIN_WORKER_ID", "CACHE_DIR"]


class Worker(_Worker):
    def __init__(self, worker_id: int = MIN_WORKER_ID,
                 workers_root: Path = WORKERS_ROOT):
        try:
            super().__init__(worker_id, workers_root, MIN_WORKER_ID)
        except WorkerError as e:
            if worker_id < MIN_WORKER_ID:
                raise WorkerError(
                    f"worker-{worker_id} belongs to the main pool (90-97). "
                    f"MCP uses {MIN_WORKER_ID} and above.") from None
            raise e
