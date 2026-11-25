#!/usr/bin/env python
"""Fail when the solver core calls getenv where it may run once per step.

    python tools/check_getenv.py            # check (exit 1 on a new call)
    python tools/check_getenv.py --list     # print every call and how it was classed

The UCRT's getenv takes the process-wide environment lock. A call on the step
path serialises every search thread: on lv22 the shadow step cost 8.5 us on one
thread and 46 us on eight, and the whole search ran no faster on 8 threads than
on 2 (2026-11-12, fixed in 2f4cce87; the same kind of call, SHSTEP_DEBUG, had
been found on 2026-11-03). The environment variables read in dp/ are
diagnosis knobs set before the process starts, so they are read once.

A call passes when it is
  * the initialiser of a static or of a namespace-scope inline variable, on the
    same line (`static const char* const e = std::getenv("X");`,
    `static const int n = [] { ... std::getenv("X") ... }();`,
    `inline const bool g_x = std::getenv("X") != nullptr;`), or
  * listed in tools/getenv_allow.txt: a call that runs once per process or per
    solver invocation (argument parsing, the end-of-call summaries). The list
    holds the file and the stripped text of the line, so it survives line moves
    but not an edit of that line.
Anything else fails: make it a static, or, when it truly runs once per call,
add the line to the allow list with the reason.
"""
from __future__ import annotations

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
SRC = [ROOT / "dp" / "src"]
ALLOW = ROOT / "tools" / "getenv_allow.txt"

CALL = re.compile(r"\bgetenv\s*\(")
# within the statement that holds the call (the text since the last ';' before it): a static or
# namespace-scope inline declaration with an initialiser, e.g. `static const char* const e = std::getenv(`
# or `static const int n = [] { const char* e = std::getenv(`
OWNER = re.compile(r"(?:^|[\s{(])(?:static|inline)\b[^=]*=")


def read_once(code: str, pos: int) -> bool:
    stmt = code[:pos].rsplit(";", 1)[-1]
    return OWNER.search(stmt) is not None


def strip_comment(line: str) -> str:
    out, i, q = [], 0, None
    while i < len(line):
        c = line[i]
        if q:
            out.append(c)
            if c == "\\" and i + 1 < len(line):
                out.append(line[i + 1]); i += 2; continue
            if c == q:
                q = None
        elif c in "\"'":
            q = c; out.append(c)
        elif line.startswith("//", i):
            break
        else:
            out.append(c)
        i += 1
    return "".join(out)


def load_allow() -> set[tuple[str, str]]:
    allow = set()
    if not ALLOW.exists():
        return allow
    for raw in ALLOW.read_text(encoding="utf-8").splitlines():
        if not raw.strip() or raw.lstrip().startswith("#"):
            continue
        f, _, text = raw.partition("\t")
        allow.add((f.strip(), text.strip()))
    return allow


def main() -> int:
    listing = "--list" in sys.argv[1:]
    allow = load_allow()
    used = set()
    bad = []
    n = 0
    for base in SRC:
        for p in sorted(base.rglob("*")):
            if p.suffix not in (".hpp", ".cpp", ".h") or p.name == "check_getenv.py":
                continue
            rel = p.relative_to(ROOT).as_posix()
            for ln, line in enumerate(p.read_text(encoding="utf-8", errors="replace").splitlines(), 1):
                code = strip_comment(line)
                calls = [m.start() for m in CALL.finditer(code)]
                if not calls:
                    continue
                n += len(calls)
                text = line.strip()
                if all(read_once(code, c) for c in calls):
                    kind = "static"
                elif (rel, text) in allow:
                    kind = "allowed"; used.add((rel, text))
                else:
                    kind = "NEW"; bad.append((rel, ln, text))
                if listing:
                    print("%-8s %s:%d  %s" % (kind, rel, ln, text[:150]))
    stale = sorted(allow - used)
    for f, t in stale:
        print("stale allow entry (no such line any more): %s  %s" % (f, t[:120]))
    for rel, ln, text in bad:
        print("getenv not read once: %s:%d  %s" % (rel, ln, text[:160]))
    print("getenv calls: %d, not read once and not allowed: %d, stale allow entries: %d" % (n, len(bad), len(stale)))
    return 1 if bad or stale else 0


if __name__ == "__main__":
    sys.exit(main())
