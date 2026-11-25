r"""seqcall_order_check.py: does a dp call depend on the calls made before it in the same process?

The mod calls dp::cliMain many times in one game (dpcore.cpp solveInProcess), and the one-session cold runs every level in
one process. A command-line run makes one call per process and can never see state that one call leaves for the next, so
"every section agrees with the game when replayed one per process" says nothing about the loop. This replays the calls of
an argv file (one call per line, seqcall --file's format) three ways and compares each call's trace bytes and verdict line:

  alone     every line in a process of its own (seqcall with that one line)
  forward   all the lines in one process, in the file's order
  reverse   all the lines in one process, last line first

Each call's --out is rewritten into a directory per arm, so the three never share a file. Exit status 0 when every call
agrees in all three, 1 otherwise (the differing calls are listed).

  python tools/seqcall_order_check.py --seqcall <seqcall.exe> [--inproc <objrects>] --file <argv.txt> --work <dir>

With --inproc the level text is handed to every call as the mod hands it (seqcall --inproc); the lines then name
"(in-process level)" as their level argument. A known case (2026-11-13): two section replays of one custom level, the second
met a teleport orb the first had passed as a plain ring (shadow_step.hpp shadowThreadOf kept a thread's world across calls).
"""
import argparse
import os
import re
import subprocess
import sys

VERDICT = re.compile(r"^REPLAY: (model DIED|SURVIVED|WINDOW END).*$", re.M)


def run(seqcall, inproc, lines, workdir, tag):
    os.makedirs(workdir, exist_ok=True)
    outs = []
    rewritten = []
    for i, line in enumerate(lines):
        toks = line.split()
        if "--out" not in toks:
            sys.exit("line %d has no --out" % (i + 1))
        j = toks.index("--out") + 1
        toks[j] = os.path.join(workdir, "%s_%d" % (tag, i))
        outs.append(toks[j])
        rewritten.append(" ".join(toks))
    argv_path = os.path.join(workdir, "%s_argv.txt" % tag)
    with open(argv_path, "w", encoding="utf-8") as f:
        f.write("\n".join(rewritten) + "\n")
    cmd = [seqcall] + (["--inproc", inproc] if inproc else []) + ["--file", argv_path]
    p = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, errors="replace")
    parts = re.split(r"(?m)^=== seqcall: invocation \d+ of \d+ ===\n", p.stdout)[1:]
    res = []
    for i, out in enumerate(outs):
        body = parts[i] if i < len(parts) else ""
        m = VERDICT.search(body)
        tp = out + ".trace.csv"
        trace = open(tp, "rb").read() if os.path.exists(tp) else None
        res.append((m.group(0) if m else None, trace))
    return res


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--seqcall", required=True)
    ap.add_argument("--inproc", default="")
    ap.add_argument("--file", required=True)
    ap.add_argument("--work", required=True)
    a = ap.parse_args()
    lines = [l.strip() for l in open(a.file, encoding="utf-8") if l.strip()]
    n = len(lines)
    alone = [run(a.seqcall, a.inproc, [lines[i]], os.path.join(a.work, "alone"), "c%d" % i)[0] for i in range(n)]
    fwd = run(a.seqcall, a.inproc, lines, os.path.join(a.work, "forward"), "c")
    rev = run(a.seqcall, a.inproc, lines[::-1], os.path.join(a.work, "reverse"), "c")[::-1]
    bad = 0
    for i in range(n):
        for name, got in (("forward", fwd[i]), ("reverse", rev[i])):
            same_line = got[0] == alone[i][0]
            same_trace = got[1] is not None and got[1] == alone[i][1]
            if not (same_line and same_trace):
                bad += 1
                print("call %d (%s): verdict %s / alone %s; trace %s" % (i + 1, name, got[0], alone[i][0], "same" if same_trace else "DIFFERS"))
    print("%d calls, alone / forward / reverse: %s" % (n, "all agree" if not bad else "%d disagreements" % bad))
    sys.exit(1 if bad else 0)


if __name__ == "__main__":
    main()
