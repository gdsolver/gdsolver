"""Every value-less switch in dp's cli.hpp is read by the loop that sees the last argument.

cli.hpp parses its argv in two kinds of loop: `for (int i = 2; i < argc; ++i)`,
which sees every argument, and `for (int i = 2; i + 1 < argc; ++i)`, which reads
argv[i + 1] and so never looks at the last one. A switch that takes no value but is
read only by the second kind is silently off whenever it is passed last -- and the
mod appends cfg `dparg` last, which is where every --replay call ends. That has
happened four times (the last on 2026-09-22, --bootretime, which made two 22-level
colds measure a half-on arm), so it is checked here rather than remembered.

A switch counts as value-less when its handler (the line, or the block or statement it opens)
never reads argv[i + 1].
It passes if the same spelling is also read inside a whole-argv loop.

usage: python py/test_valueless_flags.py [cli.hpp]   (exit 1 and the list when one is found)
"""
import re
import sys
from pathlib import Path

CLI = (Path(sys.argv[1]) if len(sys.argv) > 1 else
       Path(__file__).resolve().parents[1] / "dp" / "src" / "dp" / "cli.hpp")
LOOP = re.compile(r"for \(int i = 2; (i \+ 1 < argc|i < argc); \+\+i\)")
HANDLER = re.compile(r'!std::strcmp\(argv\[i\], "(--[^"]+)"\)')


def loop_bodies(lines: list[str]):
    """(kind, [lines]) for each parse loop, 'all' or 'argc-1', by brace depth."""
    i = 0
    while i < len(lines):
        m = LOOP.search(lines[i])
        if not m:
            i += 1
            continue
        kind = "argc-1" if m.group(1).startswith("i + 1") else "all"
        body, depth, j = [], 0, i
        started = False
        while j < len(lines):
            depth += lines[j].count("{") - lines[j].count("}")
            if "{" in lines[j]:
                started = True
            body.append(lines[j])
            if started and depth <= 0:
                break
            if not started and j > i and ";" in lines[j]:   # a one-statement loop
                break
            j += 1
        yield kind, body
        i = j + 1


def handler_text(body: list[str], k: int) -> str:
    """The handler that starts on body[k]: through its closing brace when it opens
    one, else through the first line that ends the statement."""
    out, depth = [], 0
    for ln in body[k:]:
        out.append(ln)
        depth += ln.count("{") - ln.count("}")
        if depth <= 0 and (";" in ln or "}" in ln):
            break
    return "\n".join(out)


def main() -> int:
    lines = CLI.read_text(encoding="utf-8").splitlines()
    whole, partial = set(), {}
    for kind, body in loop_bodies(lines):
        for k, ln in enumerate(body):
            for flag in HANDLER.findall(ln):
                if kind == "all":
                    whole.add(flag)
                    continue
                text = handler_text(body, k)
                if not re.search(r"argv\[(i \+ 1|\+\+i|i\+1)\]", text):
                    partial.setdefault(flag, ln.strip())
    bad = {f: ln for f, ln in partial.items() if f not in whole}
    print(f"{len(whole)} switches read by a whole-argv loop, "
          f"{len(partial)} value-less handlers in an argc-1 loop, {len(bad)} read only there")
    for f, ln in sorted(bad.items()):
        print(f"  {f:<24} {ln[:100]}")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
