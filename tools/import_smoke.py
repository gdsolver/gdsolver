#!/usr/bin/env python
"""Import every tool in py/ and mcp/ and report the ones that fail.

    python tools/import_smoke.py

A missing import or a typo in a module-level path expression is invisible
until the tool is run, which in this project can be an hour into a cold run.
This is the two-second version of that check; run it after touching paths,
imports or module-level constants.

Exit status 0 = every module imported.
"""
from __future__ import annotations

import contextlib
import importlib
import io
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "py"))
sys.path.insert(0, str(ROOT / "mcp"))

# Modules that do real work at import time (they read argv, or need a running
# worker) and therefore cannot be import-tested.
SKIP = {"run_calib", "secsolve_run", "seed_worker"}


def modules() -> list[str]:
    out = []
    for p in sorted((ROOT / "py").glob("*.py")):
        if not p.name.startswith("_") and p.stem not in SKIP:
            out.append(p.stem)
    for pkg, folder in (("gdtas", ROOT / "py" / "gdtas"),
                        ("gdmcp", ROOT / "mcp" / "gdmcp")):
        for p in sorted(folder.glob("*.py")):
            if p.stem != "__init__" and p.stem not in SKIP:
                out.append(f"{pkg}.{p.stem}")
    return out


def main() -> int:
    mods = modules()
    bad: list[tuple[str, str]] = []
    for m in mods:
        try:
            with contextlib.redirect_stdout(io.StringIO()):
                importlib.import_module(m)
        except SystemExit:
            pass                      # argparse in a module-level main guard
        except Exception as e:        # noqa: BLE001 - report, do not raise
            bad.append((m, f"{type(e).__name__}: {e}"))
    print(f"imported {len(mods) - len(bad)} / {len(mods)}")
    for m, e in bad:
        print(f"  FAIL {m}: {e}")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
