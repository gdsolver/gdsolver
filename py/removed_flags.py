"""py/removed_flags.py: the leveldp switches that were removed (dp/removed_flags.txt), for an instrument that replays a stored argv.

A sweep / flight argv written before a switch was removed still carries its spelling; leveldp refuses it by name (rc 2) rather than ignore it. An instrument that
reads such an argv (the production sync, the 604 sections) drops the removed spellings first:

    from removed_flags import drop_removed
    args = drop_removed(args)

A spelling ending in '=' is matched as a prefix. A removed switch that took values has "takes N value(s)" in its reason; its value tokens go with it.
"""
from __future__ import annotations

import re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def removed_table(root: Path = ROOT) -> dict[str, int]:
    """spelling -> number of value tokens that follow it (0 for a plain switch)."""
    out: dict[str, int] = {}
    for raw in (root / "dp" / "removed_flags.txt").read_text(encoding="utf-8").split("\n"):
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        parts = line.split(None, 3)
        reason = parts[3] if len(parts) > 3 else ""
        m = re.search(r"takes (\d+) value", reason)
        out[parts[0]] = int(m.group(1)) if m else 0
    return out


def drop_removed(args: list[str], root: Path = ROOT) -> list[str]:
    """`args` without the removed switches (and the values they took)."""
    table = removed_table(root)
    exact = {k: v for k, v in table.items() if not k.endswith("=")}
    prefixes = [k for k in table if k.endswith("=")]
    out: list[str] = []
    i = 0
    while i < len(args):
        a = args[i]
        if a in exact:
            i += 1 + exact[a]
            continue
        if any(a.startswith(p) for p in prefixes):
            i += 1
            continue
        out.append(a)
        i += 1
    return out