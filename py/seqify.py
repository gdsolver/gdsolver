"""Which attempt sequence clears a level from a fresh game: find the shortest prelude for a solved plan.

    python py/seqify.py <plan> --level 9 --pool 99
    python py/seqify.py <plan> --levelfile <level.lvl> --pool 99

A level can start differently on its first attempt (an Item Persistence / attempt-count start: the first attempt
dies, the later ones play). The solver flies its plans in a session that is already past that, so the plan it
files clears there and not necessarily from a fresh game. This replays the plan from a FRESH layer, as the plan
alone (k=0) and then after k = 1, 2, ... empty prelude attempts (`attempt=i` / `until=1` blocks, mod/plan_io.hpp),
each in a new worker session, and writes the first one that clears as `<plan>.seq.txt`. If none does, the plan
cleared only in the middle of a session: it is NOT a cold success, and this says so (exit 1).
The prelude attempts are empty: a level whose later attempts need something done in the earlier ones (an item
written by play) is not covered and shows up here as "no prelude clears".
"""
from __future__ import annotations

import argparse
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

from gdtas import plan, results
from gdtas.paths import DEFAULT_WORKER, WORKERS_ROOT
from gdtas.worker import run_session


def sequence_text(inputs: list[tuple[int, int]], k: int, lineage: str = "") -> str:
    lines: list[str] = []
    for i in range(1, k + 1):
        lines.append(f"attempt={i}")
        if lineage:
            lines.append(f"# lineage={lineage}")
        lines.append("until=1")
    lines.append(f"attempt={k + 1}")
    if lineage:
        lines.append(f"# lineage={lineage}")
    lines += [f"input={t},{d}" for t, d in inputs]
    return "\n".join(lines) + "\n"


REPLAY_CFG = ["enabled=1", "attempts=1", "quitwhendone=1", "blockinput=1", "cbs=0", "cos=1", "notrace=1",
              "fastdt=0.0166667", "fastloops=1800", "skiprender=1", "solve=0", "music=mute", "coins=0"]
# Keys of the run's own cfg a replay must not take: they make the session solve, record, serve or snapshot, or
# they say which level, plan and attempt budget -- the replay's own business. With the run's dpsolve=1 the
# judge's session became a solving session, and its no-input nodeath recording pass ran to the end and read as
# the plan's clear (three custom levels, 2026-10-09).
SOLVER_KEYS = ("capture", "servemode", "solve", "attempts", "levels", "level", "levelfile", "input", "planfile",
               "researchcapture", "nodeath")


def replay_cfg(cfg_level: list[str], plan_cfg: list[str], extra_cfg: list[str] | None) -> tuple[list[str], list[str]]:
    """(the replay's cfg, the run's keys it dropped). The run's keys come first and the replay's after, so where
    both set a key the replay's wins."""
    kept, dropped = [], []
    for c in extra_cfg or []:
        key = c.split("=", 1)[0].strip().lower()
        (dropped if key.startswith("dp") or key in SOLVER_KEYS else kept).append(c)
    return kept + REPLAY_CFG + cfg_level + plan_cfg, dropped


def replay(worker: int, cfg_level: list[str], plan_cfg: list[str], timeout_s: float, workers_root: Path,
           mod_file: Path | None = None, extra_cfg: list[str] | None = None, log=print):
    """One fresh-game replay. `mod_file` is the package the plan was SOLVED with (a collect pins one and judges
    with that one: a replay on another package is another game), `extra_cfg` the run's own cfg keys, less the
    ones that would make it anything but a replay (replay_cfg)."""
    cfg, dropped = replay_cfg(cfg_level, plan_cfg, extra_cfg)
    if dropped:
        log(f"seqify: the replay does not take the run's {', '.join(dropped)}")
    kw = {"mod_file": mod_file} if mod_file is not None else {}
    r = run_session(worker, cfg, timeout_s=timeout_s, workers_root=workers_root, **kw)
    solving = [ln for ln in r.lines if ln.startswith("dpsolve: start") or ln.startswith(results.NODEATH_PASS)]
    if solving:
        return False, f"REFUSED: the replay ran as a solving session: {solving[0][:120]}", []
    clear, detail = results.clear_verdict(r.lines)
    return clear, detail, [ln for ln in r.lines if ln.startswith("sequence:")]


def find_prelude(src: Path, cfg_level: list[str], worker: int, max_prelude: int = 3, timeout_s: float = 480.0,
                 workers_root: Path | None = None, lineage: str = "", start_k: int = 0, log=print,
                 mod_file: Path | None = None, extra_cfg: list[str] | None = None) -> int | None:
    """The shortest number of empty prelude attempts with which the plan at `src` clears from a fresh game, or None."""
    inputs = plan.read_inputs(src)
    tmp = src.with_name(src.stem + ".seqify_try.txt")
    try:
        for k in range(start_k, max_prelude + 1):
            if k == 0:
                plan_cfg = [f"input={t},{d}" for t, d in inputs]
            else:
                tmp.write_text(sequence_text(inputs, k, lineage), encoding="ascii")
                plan_cfg = [f"planfile={tmp.resolve()}"]
            clear, detail, _ = replay(worker, cfg_level, plan_cfg, timeout_s, workers_root or Path(WORKERS_ROOT),
                                      mod_file, extra_cfg, log=log)
            log(f"prelude k={k}: {'CLEAR' if clear else 'no clear'}  {detail}")
            if clear:
                if k:
                    src.with_name(src.stem + ".seq.txt").write_text(sequence_text(inputs, k, lineage), encoding="ascii")
                return k
        return None
    finally:
        tmp.unlink(missing_ok=True)


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("plan")
    ap.add_argument("--level", type=int, help="official/numbered level id")
    ap.add_argument("--levelfile", help="a .lvl file (level 9001 with levelfile=)")
    ap.add_argument("--pool", type=int, default=DEFAULT_WORKER, help="the one worker to use")
    ap.add_argument("--max-prelude", type=int, default=3)
    ap.add_argument("--start-k", type=int, default=0, help="first prelude length to try (a test aid)")
    ap.add_argument("--timeout-minutes", type=float, default=8.0)
    ap.add_argument("--workers-root", default=str(WORKERS_ROOT))
    ap.add_argument("--lineage", default="", help="written as a # lineage= comment next to each block")
    a = ap.parse_args(argv)
    if bool(a.level) == bool(a.levelfile):
        ap.error("give exactly one of --level / --levelfile")
    src = Path(a.plan)
    if plan.is_attempt_sequence(src):
        print(f"{src}: already an attempt sequence; nothing to do")
        return 0
    cfg_level = ([f"level={a.level}"] if a.level else ["level=9001", f"levelfile={a.levelfile}", "clearance=1"])
    out = src.with_name(src.stem + ".seq.txt")
    k = find_prelude(src, cfg_level, a.pool, a.max_prelude, a.timeout_minutes * 60, Path(a.workers_root), a.lineage, a.start_k)
    if k is not None:
        if k == 0:
            print("seqify: the plan clears from a fresh game as it is (no prelude needed)")
        else:
            print(f"seqify: clears from a fresh game after {k} prelude attempt(s) -> {out}")
        return 0
    print(f"seqify: NO prelude of 0..{a.max_prelude} empty attempts clears {src.name} from a fresh game. If the plan "
          f"cleared in the session that solved the level, it did so after earlier attempts that were more than an "
          f"empty prelude (or the plan does not clear at all): it is NOT a cold success")
    return 1


if __name__ == "__main__":
    sys.exit(main())
