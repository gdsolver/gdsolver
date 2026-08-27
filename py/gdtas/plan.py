"""Reading and writing plan files (`input=<tick>,<0|1>`).

NEVER ADD A BOM. The MOD's `input=` test is a prefix match, so with a BOM the first
line becomes `﻿input=...`, the test fails, and THE FIRST INPUT SILENTLY DISAPPEARS.
Nothing looks wrong in the log; it only shows up as "only the first jump never fires".
When reading, conversely, always use utf-8-sig: existing solution files written by
PowerShell do have a BOM.
"""

from __future__ import annotations

import re
from pathlib import Path

_INPUT_RE = re.compile(r"\s*input=(-?\d+)\s*,\s*(\d+)")


def read_inputs(path: Path | str) -> list[tuple[int, int]]:
    """Read a plan as a sequence of (tick, down), sorted by ascending tick.

    Raises if an `input=` line cannot be parsed. Dropping such lines silently is
    exactly how the BOM case above hides: only one line disappears.
    """
    out: list[tuple[int, int]] = []
    skipped = 0
    for line in Path(path).read_text(encoding="utf-8-sig", errors="replace").splitlines():
        m = _INPUT_RE.match(line)
        if m:
            out.append((int(m.group(1)), 1 if int(m.group(2)) else 0))
        elif "input=" in line:
            skipped += 1
    if skipped:
        raise ValueError(f"{path}: {skipped} lines contain `input=` but could "
                         f"not be parsed")
    out.sort(key=lambda p: p[0])
    return out


def read_input_lines(path: Path | str) -> list[str]:
    """Return the `input=...` lines raw (for putting straight into autorun.cfg)."""
    return [f"input={t},{d}" for t, d in read_inputs(path)]


def format_plan(inputs: list[tuple[int, int]],
                injects: list[dict] | None = None,
                stop_at: int | None = None) -> str:
    """Assemble the body to put into plan_in.txt / autorun.cfg.

    inject is [{"tick": T, "x": .., "y": .., "vy": ..}, ...] (the MOD's loadPlanExtras).
    """
    lines = [f"input={t},{1 if d else 0}" for t, d in inputs]
    for s in injects or []:
        if "tick" not in s:
            raise ValueError(f"inject has no tick: {s}")
        parts = [str(int(s["tick"]))]
        for k in ("x", "y", "vy"):
            if s.get(k) is not None:
                parts.append(f"{k}={float(s[k])}")
        if len(parts) == 1:
            raise ValueError(f"inject has none of x/y/vy: {s}")
        lines.append("inject=" + ",".join(parts))
    if stop_at is not None:
        lines.append(f"stopat={int(stop_at)}")
    return "\n".join(lines)


def write_plan(path: Path | str, text: str) -> None:
    """Write UTF-8 without a BOM (do not add writers that bypass this function)."""
    Path(path).write_bytes(text.encode("utf-8"))


def write_plan_from_file(dst: Path | str, src: Path | str) -> str:
    """Copy an existing plan file to dst with the BOM stripped. Returns the body written."""
    text = format_plan(read_inputs(src))
    write_plan(dst, text)
    return text
