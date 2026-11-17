"""Reading and writing plan files (`input=<tick>,<mask>`; the mask is 0 or 1 for a plan of one button, 2 and 3 carry player 2's).

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
MASK_DRIVEN = 3   # bit 0: player 1's jump, bit 1: player 2's (mod/plan_io.hpp pushInputMask)


def is_attempt_sequence(path: Path | str) -> bool:
    """True when the file holds an attempt sequence: `attempt=<N>` blocks (mod/plan_io.hpp). Its blocks are
    several attempts of ONE layer, run in order; flattened into one plan they would be a different plan."""
    for line in Path(path).read_text(encoding="utf-8-sig", errors="replace").splitlines():
        if line.startswith("attempt="):
            return True
    return False


def read_input_masks(path: Path | str) -> list[tuple[int, int]]:
    """Read a plan as a sequence of (tick, mask), sorted by ascending tick. The mask is the buttons held from that
    tick: bit 0 player 1's jump, bit 1 player 2's (a two-player level's second button); the others are reserved.

    Raises if an `input=` line cannot be parsed, or holds a bit this version does not drive. Dropping such lines silently is
    exactly how the BOM case above hides: only one line disappears.
    Raises on an attempt sequence too: reading its blocks as one plan would silently merge attempts. Hand
    such a file to the mod as `planfile=<path>` (verify_solutions does).
    """
    if is_attempt_sequence(path):
        raise ValueError(f"{path}: an attempt sequence (attempt= blocks), not a single plan; "
                         f"use is_attempt_sequence() and give it to the mod as planfile=")
    out: list[tuple[int, int]] = []
    skipped = 0
    for line in Path(path).read_text(encoding="utf-8-sig", errors="replace").splitlines():
        m = _INPUT_RE.match(line)
        if m:
            mask = int(m.group(2))
            if mask & ~MASK_DRIVEN:
                raise ValueError(f"{path}: `{line.strip()}` holds buttons no version of the mod drives (mask {mask})")
            out.append((int(m.group(1)), mask))
        elif "input=" in line:
            skipped += 1
    if skipped:
        raise ValueError(f"{path}: {skipped} lines contain `input=` but could "
                         f"not be parsed")
    out.sort(key=lambda p: p[0])
    return out


def read_inputs(path: Path | str) -> list[tuple[int, int]]:
    """Read a plan as a sequence of (tick, down) for player 1, sorted by ascending tick.

    Refuses a plan that carries player 2's button (bit 1): every reader of this form splices, cuts or rewrites the
    plan as one button, and would hand the mod a plan with player 2's presses silently gone (and, before, its
    mask read as a press of player 1). A tool that can carry both buttons reads read_input_masks.
    """
    out = read_input_masks(path)
    if any(m & 2 for _, m in out):
        raise ValueError(f"{path}: the plan carries player 2's button (input bit 1); this reader keeps one button "
                         f"and would drop it -- use read_input_masks")
    return out


def read_input_lines(path: Path | str) -> list[str]:
    """Return the `input=...` lines raw (for putting straight into autorun.cfg). A mask with bit 1 is passed on as it is:
    the launch checks that the package reads it (worker.check_plan_buttons)."""
    return [f"input={t},{d}" for t, d in read_input_masks(path)]


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
