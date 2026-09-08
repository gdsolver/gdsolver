"""Join a verified head to a re-solved tail at one tick.

reach_check's --verify arm solves a tail from an anchor and then has to hand GD
something GD can actually play: a plan that starts at t=0. This does that join.
Everything at or before `cut` comes from the head plan, everything after it from
the tail, and the two are written out as one input list.

The bare DP verdict cannot see a phantom route -- a plan the model believes and
the game refutes. Splicing and replaying is what exposes one, which is why
reach_check.py:91 records a case (lv9 at t=8,400, the kLandTol band) that "comes
out SOLVED either way" and earns its keep only under --verify.

That arm has never run. reach_check.py has called this file since a6dd210, this
repository's first commit, and the only copy of it lived outside the tree, so
every --verify run returned SKIP and exit 0 for eleven days before anyone read
the reason.

    python py/splice_plan.py <head> <tail> <cut-tick> <out> [tick-shift]

`tick-shift` is added to the tail's ticks, for a tail solved in its own frame
rather than the level's.
"""
import re
import sys
from pathlib import Path

LINE = re.compile(r"input=(\d+),(\d)")


def read(path: Path) -> list[tuple[int, int]]:
    """Every `input=<tick>,<state>` line, in file order.

    Anything else in the file is ignored rather than rejected: plan files carry
    comments and the occasional header, and a splice that refused them would
    fail on inputs the solver itself writes. utf-8-sig because a plan written on
    this platform can carry a BOM, and a BOM read as utf-8 silently eats the
    first line -- which shows up much later as "the first jump did not fire".
    """
    out = []
    with path.open(encoding="utf-8-sig") as f:
        for ln in f:
            m = LINE.match(ln.strip())
            if m:
                out.append((int(m.group(1)), int(m.group(2))))
    return out


def main(argv: list[str]) -> int:
    if not 4 <= len(argv) <= 5:
        print(__doc__.strip().splitlines()[-3].strip(), file=sys.stderr)
        return 2
    head_p, tail_p, out_p = Path(argv[0]), Path(argv[1]), Path(argv[3])
    cut = int(argv[2])
    shift = int(argv[4]) if len(argv) == 5 else 0

    for p in (head_p, tail_p):
        if not p.exists():
            print(f"splice_plan: missing {p}", file=sys.stderr)
            return 1

    head = [x for x in read(head_p) if x[0] <= cut]
    tail = [(t + shift, s) for t, s in read(tail_p) if t > cut]
    # An empty side is not an error -- a cut before the first input or after the
    # last one is a legitimate degenerate splice -- but it is worth SAYING, since
    # a silently empty tail replays as "the head, and then nothing", which looks
    # like a plan that simply stops rather than one that was never joined.
    if not head:
        print(f"splice_plan: head is empty at cut={cut}", file=sys.stderr)
    if not tail:
        print(f"splice_plan: tail is empty at cut={cut}", file=sys.stderr)

    out_p.parent.mkdir(parents=True, exist_ok=True)
    with out_p.open("w", encoding="utf-8", newline="\n") as f:
        for t, s in head + tail:
            f.write(f"input={t},{s}\n")

    print(f"head {len(head)} (last {head[-1] if head else None})  "
          f"tail {len(tail)} (first {tail[0] if tail else None})  -> {out_p}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
