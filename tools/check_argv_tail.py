#!/usr/bin/env python
"""Find value-less flags parsed in an `i + 1 < argc` loop of dp/src/dp/cli.hpp.

    python tools/check_argv_tail.py [path/to/cli.hpp]

The argument loops that read a value stop one short of argc (`i + 1 < argc`), so they never
see the LAST argument. A flag that takes no value belongs in an `i < argc` loop; put in an
`i + 1 < argc` one it is read everywhere except when it is passed last -- and the mod appends
the cfg `dparg=` flags last, so on the resim / recorder passes (whose argv ends with them)
the last of those flags was silently dropped, while the solve calls, which carry more
arguments after them, read it. That is how --fixreach never took effect in the loop and
--bootauto only on the solve calls (2026-10-24).

For every flag handled in an `i + 1 < argc` loop the text up to the next flag handler must
read argv[i + 1]; a flag whose handler does not is reported. Exit status 0 = none.
"""
from __future__ import annotations

import re
import sys
from pathlib import Path

LOOP = re.compile(r"for \(int i = \d+; i \+ 1 < argc; \+\+i\)")
FLAG = re.compile(r'std::strcmp\(argv\[i\], "(--[A-Za-z0-9\-]+)"\)')


def loop_extent(lines: list[str], start: int) -> int:
    """Index one past the body of the loop that starts at `start`."""
    if lines[start].rstrip().endswith("{"):
        depth = lines[start].count("{") - lines[start].count("}")
        k = start + 1
        while k < len(lines) and depth > 0:
            depth += lines[k].count("{") - lines[k].count("}")
            k += 1
        return k
    k = start + 1
    while k < len(lines) and not lines[k - 1].rstrip().endswith((";", "}")):
        k += 1
    return k


def check(path: Path) -> list[tuple[int, str]]:
    lines = path.read_text(encoding="utf-8", errors="replace").split("\n")
    bad: list[tuple[int, str]] = []
    for st, line in enumerate(lines):
        if not LOOP.search(line):
            continue
        en = loop_extent(lines, st)
        marks = [(k, FLAG.findall(lines[k])[0]) for k in range(st, en) if FLAG.search(lines[k])]   # one handler per line
        for n, (k, flag) in enumerate(marks):
            stop = marks[n + 1][0] if n + 1 < len(marks) else en
            seg = "\n".join(lines[k:max(stop, k + 1)])
            if "argv[i + 1]" not in seg and "argv[i+1]" not in seg and "i + 4 < argc" not in seg:
                bad.append((k + 1, flag))
    return bad


def main() -> int:
    path = Path(sys.argv[1]) if len(sys.argv) > 1 else \
        Path(__file__).resolve().parent.parent / "dp" / "src" / "dp" / "cli.hpp"
    bad = check(path)
    for line, flag in bad:
        print(f"{path.name}:{line}: {flag} takes no value but is parsed in an `i + 1 < argc` loop "
              f"(it is never seen when passed last); move it to an `i < argc` loop")
    if not bad:
        print("check_argv_tail: ok (no value-less flag in an `i + 1 < argc` loop)")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
