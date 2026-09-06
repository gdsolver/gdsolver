"""fidelity_diff's model side alone, against GD dumps already on disk.

fidelity_diff replays a known-good plan in both the model and GD and prints the
first diverging tick. The GD half needs a worker, which rules it out while a cold
run owns one (rule 4: one cold run at a time, in its own --data-dir) -- and that
is exactly when a dp edit most wants measuring.

GD's replay of a fixed plan is deterministic, so the dumps fidelity_diff leaves
in build/fidelity stay valid ground truth for as long as only the model changes.
This runs the model half against them: same table, no worker, ~15 s for all 22.

Everything except "run GD" is fidelity_diff's own code, imported rather than
copied, so the two cannot drift apart -- same --groups order, same goal cut, same
diff. Re-run fidelity_diff itself whenever the plans or the mod change; this is
only valid against dumps taken from the plans it is replaying now.

    python py/fidelity_offline.py                # all 22
    python py/fidelity_offline.py 16 22          # two levels

A SECOND ARM, under one switched rule, is the same command with a name of its
own and the flag appended -- and, because it goes through model_replay, it gets
a sidecar saying so:

    python py/fidelity_offline.py 20 --out-prefix satrot --extra-flag=--no-satrotraw

The `=` is not decoration: `--extra-flag --no-satrotraw` is read by argparse as
an option, not as its value. The trace lands beside the default one
(build/fidelity/satrot_lv20.trace.csv) rather than overwriting it, so the two
arms can be compared afterwards.

NUMBERS TAKEN THROUGH HERE BEFORE 747dadf ARE WRONG FOR lv22. Until that commit
this called model_replay without whole_run, so --rotqueue never reached the
solver, and lv22 came out with its first divergence at 6,315 -- 365 ticks early
against the true 6,680 -- and the model stopped at 6,350 instead of 11,539. Any
lv22 figure quoted from an earlier offline run is a different run's, and the
divergence it names is not there. Other levels were unaffected (lv16 is
identical across the fix, digit for digit).
"""
from __future__ import annotations

import argparse
import sys
from pathlib import Path

_PY = Path(__file__).resolve().parent
sys.path.insert(0, str(_PY))
sys.path.insert(0, str(_PY.parent / "mcp"))

import fidelity_diff as F                                  # noqa: E402
from gdmcp.data import diff_trace                          # noqa: E402
from gdtas.paths import DATA, LEVELDP_EXE                  # noqa: E402


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("levels", nargs="*", type=int,
                    help="level numbers (default: 1..22)")
    ap.add_argument("--dumps", default=str(_PY.parent / "build" / "fidelity"),
                    help="where fidelity_diff left its dumps")
    ap.add_argument("--plans", default=str(DATA / "solution_lv{}_dp.txt"),
                    help="path to the plan; {} is replaced by the level number")
    ap.add_argument("--tol", type=float, default=0.3)
    ap.add_argument("--leveldp", default=str(LEVELDP_EXE))
    ap.add_argument("--out-prefix", default="off",
                    help="trace basename, <prefix>_lv<N> (default: off)")
    ap.add_argument("--extra-flag", action="append", default=[],
                    metavar="FLAG",
                    help="extra leveldp flag, repeatable; write it as "
                         "--extra-flag=--no-satrotraw, or argparse reads the "
                         "value as an option of its own")
    a = ap.parse_args()

    fid = Path(a.dumps)
    levels = a.levels or list(range(1, 23))
    missing = 0
    # Collected only to render fidelity_diff's per-half block below. This half of
    # the instrument was blind in the same way the main table was: "div ticks" is
    # one number for both players and three quantities (see F.PER_HALF_NOTE).
    per: list[F.Result] = []
    print(f"fidelity (model side only, dumps from {fid}, tol={a.tol})")
    if a.extra_flag:
        print(f"extra leveldp flags: {' '.join(a.extra_flag)}")
    print(f"{'lv':<5}{'first div t=':<15}{'x=':<11}{'mode':<8}{'dy':<9}"
          f"{'dvy':<9}{'div ticks':<11}{'model/cut':<14}note")
    print("-" * 92)
    for lv in levels:
        plan = Path(a.plans.replace("{}", str(lv)))
        dump = fid / f"fid_lv{lv}.dump.csv"
        if not plan.exists() or not dump.exists():
            print(f"lv{lv:<3} no plan or no dump ({plan.name} / {dump.name})")
            missing += 1
            continue
        # whole_run=True. This replays from t=0, which is the case
        # whole_run_args exists for -- its docstring says the flags are "right
        # for a run STARTING AT t=0 and wrong at an anchor", and fidelity_diff,
        # the other entrance to the same path, already passes True. Leaving it
        # False cost lv22 specifically: without --rotqueue the model stops at
        # t=6,350 with two mismatched frame/gravity transitions, and with it
        # t=6,315 agrees on both axes and the run reaches t=6,375. So every lv22
        # number measured through this entrance was a different run's.
        trace, died, _ = F.model_replay(lv, plan, fid / f"{a.out_prefix}_lv{lv}",
                                        Path(a.leveldp), False, whole_run=True,
                                        extra_flags=a.extra_flag)
        # goal_x is a GD-session number and there is none here; gd_cut_tick's
        # fallback finds the frozen tail in the dump itself, which is the same
        # tick for a level the plan clears.
        cut = F.gd_cut_tick(dump, 0.0)
        d = diff_trace(trace, dump, t1=cut, tol=a.tol, limit=10 ** 9)
        if "error" in d:
            print(f"lv{lv:<5}ERROR {d['error']}")
            continue
        rows = d["rows"]
        per.append(F.Result(level=lv, per_half=d.get("per_half", {})))
        note = f"model died t={died}" if died >= 0 else ""
        mc = f"{d['model_ticks']}/{cut if cut else d['gd_ticks']}"
        if rows:
            r = rows[0]
            print(f"lv{lv:<3}  {r[0]:<15}{r[1]:<11.1f}{str(r[4]):<8}"
                  f"{r[8]:<+9.3f}{r[9]:<+9.3f}{len(rows):<11}{mc:<14}{note}")
        else:
            print(f"lv{lv:<3}  {'(no divergence)':<13}{'':<11}{'':<8}{'':<9}{'':<9}"
                  f"{0:<11}{mc:<14}{note}")
    print("model/cut = ticks the model replayed / the tick the GD side is cut at")
    if per:
        print()
        print(F.format_per_half_table(per, a.tol))
    return 1 if missing else 0


if __name__ == "__main__":
    raise SystemExit(main())
