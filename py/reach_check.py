# -*- coding: utf-8 -*-
"""Reachability spot check (minutes). CATCHES REGRESSIONS A REPLAY CANNOT SEE.

quick_regress only looks at "can the model reproduce a verified plan" =
fidelity. On 2026-08-17, twice in a single day, REACHABILITY was broken while
that kept PASSing:

  - the dual p2 rules were made to act on a single player as well -> in the
    rotation section of lv22 the left shaft at t=6,010 became unreachable and
    the cold run was pinned at 8+ iterations
  - the m=2 anchor of cubeExit was applied to a forward-facing cube -> the same
    place dies in the same shape

Neither of them shows up in "the replay of the final plan" (the plan does not
take that branch), so a replay-type harness cannot detect them in principle.
Here we MAKE THE DP ACTUALLY SOLVE OVER A SHORT HORIZON and look at whether it
comes out SOLVED. No GD and no worker required.

Cases are placed at the entrance of "the move that is unavoidable if you are to
get through that section". The baseline is data/gdref/reach.json (updated with
--bless).

ALWAYS RE-BLESS WHEN THE SOLUTION CHANGES. A case solves the current plan with
the gdref state at that tick as its anchor, so if the solution takes a
different route the same tick becomes a different situation. On 2026-08-17,
after lv22 was swapped for the 43-iteration version, 22@5400 went
SOLVED -> PARTIAL; a physics regression was suspected and the immediately
preceding change was reverted, but it reproduced even after the revert = the
cause was a stale baseline (gdref and the quick_regress baseline had been
re-recorded, but this one was forgotten).
ANCHORS IN ROTATION SECTIONS ARE WEAK IN THIS TOOL: the one-shot state of 2900
(--spentrot) and the phase of autonomous triggers (--trigraw) are not passed,
so the frontier can die right after the anchor. Read cases placed in a rotation
section with that caveat in mind.

    python py/reach_check.py               # every case
    python py/reach_check.py --bless       # save the current result as baseline
    python py/reach_check.py --leveldp <exe>   # A/B with a differential build
"""
from __future__ import annotations

import argparse
import json
import re
import subprocess
import sys
import time
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

from gdtas import runtmp             # noqa: E402  private tmp + trace fingerprints
from gdtas.paths import LEVELDP_EXE  # noqa: E402
from quick_regress import (DATA, LEVEL_DATA, REF, groups_args, has_grouped_colliders,  # noqa: E402
                           plan_of, read_ref, start_fields)

# (level, anchor tick, horizon ticks, description[, cap]).
# Place the anchor BEFORE "the point where the frontier dies when it is broken":
# what we want to see is whether the branch entering that section is born.
# cap defaults to 2000. [2026-08-19]: target 5 (bringing the 2900 conversion vy
# in line with the GD implementation, so that with modY=0 t=4,665 becomes a
# standing start) changed the bounce phase in the vertical shaft, and with the
# cut-off bias of cap 2000 (insertion order, known) 22@3300 keeps no branch that
# reaches the conversion window (world x 6,845-6,905) and comes out PARTIAL.
# With cap 8000 it is SOLVED and the GD verification is OK. The production cold
# run uses a far larger cap.
CASES = [
    # the rotating ball/cube section of lv22. Both regressions of 2026-08-17
    # showed up here, in the shape of the left shaft at t=6,010 (x=8,303)
    # becoming unreachable.
    (22, 5400, 1200, "rotated section (frame/rev + ceiling underside + m=2 launch)"),
    # ...and one more RIGHT BEFORE THE DEATH POINT. The case above (600 ticks
    # earlier) passes even on a broken build: from a distant anchor the DP finds
    # a different route and GD accepts it too, so the phantom route never gets
    # chosen. Placed A FEW DOZEN TICKS BEFORE THE CONTACT THE RULE ACTS ON, the
    # phantom that the broken physics creates becomes the deepest as it is.
    (22, 5950, 500, "rotated ball climbing (just before the spike ceiling at y=1,095)"),
    # the first dual of lv16 (x=10,551..15,103). Always run this after touching
    # a p2 rule.
    (16, 7000, 1400, "entry to the first dual (p2 landing and hanging)"),
    # the late dual of lv16 (includes the spike row at t=8,80x that was the wall)
    (16, 8400, 1200, "mid dual (the spike row)"),
    # the swing section of lv22 (terminal clamp, slope exit). How cap 8000 came
    # about is above
    (22, 3300, 1300, "swing section (terminal clamp |vy|<=8 + launch)", 8000),
    # the inverted-ball section of lv9 (underside of the slab at x~11,265).
    # kLandTol 10.5 built a phantom route here -- "land 10.2px past the
    # underside and slip through" -- and left the cold run STUCK (2026-08-18).
    # The bare verdict comes out SOLVED either way, so this case really earns
    # its keep under --verify (where GD kills the phantom route).
    (9, 8400, 800, "inverted ball under the slab (the kLandTol band)"),
]


def solve_case(lv: int, t0: int, horizon: int, exe: Path, tmp: runtmp.RunTmp,
               threads: int, cap: int = 2000) -> dict:
    plan = plan_of(lv, str(DATA / "solution_lv{}_dp.txt"))
    gd = read_ref(lv)
    objrects = LEVEL_DATA / f"objrects_lv{lv}.txt"
    if not plan.exists() or not gd or t0 not in gd:
        return {"verdict": "SKIP", "note": "no solution or no reference"}
    if has_grouped_colliders(objrects) and not groups_args(plan):
        return {"verdict": "SKIP", "note": "no groups"}
    r = gd[t0]
    # `reach_lv{lv}_{t0}` carries NO RUN IDENTITY, and --tmp defaulted to a
    # machine-wide C:\GDtmp\reach, so two runs wrote the same paths. Same hole
    # and same fix as fixcensus / quick_regress / verify: private by default,
    # fingerprinted when a --tmp is named by hand. All three suffixes are
    # reserved together because they belong to one case -- the .txt written
    # here, and the two gd_survives writes below. gdtas/runtmp.py.
    base = tmp.reserve(f"reach_lv{lv}_{t0}", ".txt", ".spliced.txt", ".dump.csv")
    out = Path(str(base) + ".txt")
    a = [str(exe), str(objrects), "--out", str(out),
         "--cap", str(cap), "--shipyq", "0.25", "--shipvq", "1.0",
         "--threads", str(threads),
         "--start", start_fields(t0, r, plan, gd.get(t0 - 1)),
         "--horizon", str(horizon)]
    if r.get("pmin") and r.get("pmax"):
        a += ["--startband", f"{r['pmin']},{r['pmax']}"]
    trig, grp = LEVEL_DATA / f"triggers_lv{lv}.txt", LEVEL_DATA / f"objgroups_lv{lv}.txt"
    if trig.exists() and grp.exists():
        a += ["--triggers", str(trig), "--objgroups", str(grp)]
    obb = LEVEL_DATA / f"obb_lv{lv}.txt"
    if obb.exists():
        a += ["--obb", str(obb)]
    a += groups_args(plan)
    t = time.time()
    p = subprocess.run(a, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL,
                       text=True, encoding="utf-8", errors="replace")
    verdict, deep_t, deep_x = "FAILED", -1, -1.0
    resim = None
    for ln in (p.stdout or "").splitlines():
        m = re.match(r"^(PARTIAL|FAILED): frontier died at t=(\d+) x=([\d.\-]+)",
                     ln)
        if m:
            verdict, deep_t, deep_x = m.group(1), int(m.group(2)), float(m.group(3))
            continue
        # The solver's own answer to "does the plan it just emitted survive its
        # own walk?" -- see the `resimdie:` line in cli.hpp. Measured on lv22 at
        # twelve anchors, SEVEN final plans contain ticks the model calls a kill,
        # four of them in runs that printed SOLVED. Nothing here changes a
        # verdict on it: whether a fired tick should invalidate a plan depends on
        # how often the model overkills, which has not been measured. But this
        # harness reads two patterns out of a solver that prints a dozen, and
        # every one it drops (capstat: among them) is a question nobody gets to
        # ask. Carry it, print it, decide later.
        m = re.match(r"^resimdie: dead=(-?\d+) of=(-?\d+) first=(-?\d+) "
                     r"last=(-?\d+) contig=(\d)", ln)
        if m:
            resim = {"dead": int(m.group(1)), "of": int(m.group(2)),
                     "first": int(m.group(3)), "last": int(m.group(4)),
                     "contig": int(m.group(5))}
            continue
        if ln.startswith("SOLVED at") and verdict != "PARTIAL":
            verdict = "SOLVED"
    # Fingerprint it as ours the moment our solver has let go of it. Absent is
    # recorded as absent, so a tail that appears later cannot be read as this
    # run's.
    tmp.claim(out)
    return {"verdict": verdict, "deep_t": deep_t, "deep_x": deep_x,
            "seconds": round(time.time() - t, 1), "tail": out, "t0": t0,
            "level": lv, "plan": plan, "resim": resim}


def gd_survives(res: dict, horizon: int, worker_id: int, tmp: runtmp.RunTmp,
                timeout_s: float) -> dict:
    """Splice the solved tail onto the verified prefix and RUN IT ONCE THROUGH GD.

    This is what catches the regressions a replay-type harness cannot see in
    principle: broken physics builds "a phantom route that GD kills", so the
    symptom only appears on A PLAN THE MODEL BUILT ITSELF. Measured 2026-08-17:
    the build that made the dual rules act on a single player passed both
    quick_regress and the bare reach (verdict SOLVED), while the tail built here
    dies in GD at t=6,010. The same check as one iteration of the driver's loop.
    """
    # lazy imports (keep the GD-free path light)
    from fidelity_diff import gd_cut_tick, gd_replay
    lv, t0 = res["level"], res["t0"]
    tail = Path(str(res["tail"]))
    if not tail.exists():
        return {"gd": "SKIP", "note": "no tail was produced"}
    # solve_case already reserved these two alongside the tail.
    spliced = tmp.dir / f"reach_lv{lv}_{t0}.spliced.txt"
    r = subprocess.run([sys.executable, str(Path(__file__).parent / "splice_plan.py"),
                        str(res["plan"]), str(tail), str(t0), str(spliced)],
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    if r.returncode != 0 or not spliced.exists():
        return {"gd": "SKIP", "note": "splice failed"}
    tmp.claim(spliced)
    dump = tmp.dir / f"reach_lv{lv}_{t0}.dump.csv"
    from gdtas.paths import WORKERS_ROOT
    try:
        # [2026-09-07] `goal_x` IS BOUND BECAUSE THE VERDICT BELOW NEEDS IT.
        # gd_replay returns (clear, detail, goal_x) and every field used to be
        # thrown away, which left the verdict to be read off the dump's final
        # row -- and THE DUMP DOES NOT END WHERE THE RUN ENDS. GD does not
        # truncate on death, it PADS: it keeps writing frozen rows (identical
        # x and y) afterwards, so the final tick is the end of the padding.
        # `last >= t0 + horizon` therefore cleared almost any target and could
        # barely produce a DEAD at all. fidelity_diff.gd_cut_tick already knows
        # this (its docstring measures lv1: t=20329..21592 frozen at the goal)
        # and it needs exactly this goal_x.
        # `detail` STAYS DISCARDED, deliberately: it is what marks a timed-out
        # session (`if res.timed_out and not clear: detail = f"TIMEOUT (...)"`).
        # A timeout truncates the dump EARLY, so it can only turn a surviving
        # tail into a DEAD -- a phantom reported that is not there. Acting on
        # it is a second change and not the one that was approved.
        _clear, _detail, goal_x = gd_replay(worker_id, lv, spliced, dump,
                                            timeout_s, WORKERS_ROOT)
    except Exception as e:                     # a missing worker must not kill the check
        return {"gd": "SKIP", "note": f"{type(e).__name__}"}
    tmp.claim(dump)
    # where the run ACTUALLY ended: the goal if it was reached, otherwise the
    # first row of the frozen stretch at the end.
    target = t0 + horizon
    cut = gd_cut_tick(dump, goal_x)
    if cut is not None:
        return {"gd": "OK" if cut >= target else "DEAD", "gd_last": cut,
                "gd_target": target, "gd_cut": cut}
    # NO CUT. gd_cut_tick returns None when the dump cannot be read, when no row
    # parses, or when the final row's x differs from the one before it -- i.e.
    # when there is no frozen tail at all. The padding this change exists to
    # defeat is by construction absent there, so the final row IS the end of the
    # run and the old attempt-max walk is the right reading, not a silent
    # relapse. It is kept as the fallback, and `gd_cut: None` records that the
    # branch was taken.
    last = -1
    try:
        with dump.open(encoding="utf-8-sig", errors="replace") as f:
            hdr = f.readline().rstrip("\n").split(",")
            it, ia = hdr.index("tick"), hdr.index("attempt")
            amax = -1
            for ln in f:
                q = ln.split(",")
                if len(q) <= max(it, ia):
                    continue
                at = int(float(q[ia] or 0))
                if at > amax:
                    amax, last = at, -1
                if at == amax:
                    last = int(float(q[it] or 0))
    except OSError:
        return {"gd": "SKIP", "note": "cannot read the dump"}
    return {"gd": "OK" if last >= target else "DEAD", "gd_last": last,
            "gd_target": target, "gd_cut": None}


def main(argv=None) -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--leveldp", default=str(LEVELDP_EXE))
    # WAS r"C:\GDtmp\reach" -- machine-wide, shared by every checkout and every
    # concurrent run, with file names that carry no run identity. Unset, the run
    # now gets a directory only it can name; name one by hand and it behaves as
    # before, with the fingerprints making the sharing detectable.
    ap.add_argument("--tmp", default=None)
    ap.add_argument("--threads", type=int, default=8)
    ap.add_argument("--parallel", type=int, default=2)
    ap.add_argument("--bless", action="store_true")
    ap.add_argument("--only", type=int, nargs="*", default=None,
                    help="limit to these levels")
    ap.add_argument("--verify", action="store_true",
                    help="replay each solved tail once in GD to confirm it "
                         "survives (needs a worker)")
    ap.add_argument("--worker-id", type=int, default=99)
    ap.add_argument("--timeout-minutes", type=float, default=6.0)
    a = ap.parse_args(argv)
    tmp = runtmp.RunTmp("reach_check", a.tmp, root=DATA / "tmp_reachcheck")
    try:
        return _run(a, tmp)
    finally:
        tmp.release()


def _run(a, tmp: runtmp.RunTmp) -> int:
    """main's body, split out so the run directory has ONE lifetime instead of
    a release repeated on each of four return paths (the same reasoning
    fixcensus gives for its own split)."""
    base_path = REF / "reach.json"
    base = json.loads(base_path.read_text()) if base_path.exists() else {}

    cases = [c for c in CASES if a.only is None or c[0] in a.only]
    t0 = time.time()
    with ThreadPoolExecutor(max_workers=a.parallel) as ex:
        futs = [(c, ex.submit(solve_case, c[0], c[1], c[2],
                              Path(a.leveldp), tmp, a.threads,
                              c[4] if len(c) > 4 else 2000))
                for c in cases]
        results = [(c, f.result()) for c, f in futs]

    # run the GD verification SERIALLY (one worker, so result.txt cannot get mixed)
    if a.verify:
        for (lv, at, hz, note, *_), r in results:
            if r["verdict"] == "SKIP":
                continue
            r.update(gd_survives(r, hz, a.worker_id, tmp,
                                 a.timeout_minutes * 60))

    # BEFORE THE TABLE AND BEFORE --bless. A tail another run wrote would move
    # a verdict and a blessed baseline would then carry it. Same refusal and
    # same exit 2 as fixcensus / quick_regress / verify.
    clobbered = tmp.check()
    if clobbered:
        text = runtmp.banner(clobbered, "reach_check", tmp.dir)
        print(text)
        print(text, file=sys.stderr)
        return 2
    # On the clean path say so, with the count: a guard whose success is silent
    # cannot be told from one that never ran. stderr, so a guarded run stays
    # byte-comparable with an unguarded one on stdout.
    print(f"[runtmp] {tmp.count()} artefacts verified as ours -- {tmp.dir}"
          f"{' (private)' if tmp.private else ''}", file=sys.stderr)

    print(f"{'case':<34} {'verdict':<8} {'deepest':<22} {'GD':<6} "
          f"{'s':<6} plan's own walk")
    bad, now = [], {}
    for (lv, at, hz, note, *_), r in results:
        key = f"{lv}@{at}"
        # what goes into the baseline is the pair "DP verdict + GD survival".
        # A run that did not drive GD only updates the DP verdict (the GD field
        # does not damage the baseline).
        now[key] = r["verdict"] + (f"/{r['gd']}" if r.get("gd") else "")
        deep = ("-" if r.get("deep_t", -1) < 0
                else f"t={r['deep_t']} x={r.get('deep_x', -1):.0f}")
        gd_tag = r.get("gd", "-")
        if gd_tag == "DEAD":
            gd_tag = f"DEAD@{r.get('gd_last')}"
        # A BARE "SKIP" CANNOT SAY WHETHER THE TOOL HAD NOTHING TO DO OR COULD
        # NOT RUN. Six sites return SKIP and each records a distinct `note`,
        # but the note was never printed, so "this case does not apply" and "a
        # hard dependency is missing" rendered identically. --verify has been
        # unable to run since a6dd210, this repository's FIRST commit
        # (py/splice_plan.py is called at :169 and has never been added to this
        # tree), and eleven days of runs reported three clean SKIPs and exit 0.
        # The reason was in r["note"] the whole time. Printing it does not fix
        # --verify; it stops the instrument being unable to report its own
        # unavailability, which is why nobody noticed.
        skip_why = f"  ({r['note']})" if gd_tag == "SKIP" and r.get("note") else ""
        # "?" is not "clean". A missing resimdie: line means the solver predates
        # the counter or the walk never ran, and those must not render as a plan
        # that survived -- the same distinction a bare SKIP failed to make above.
        rs = r.get("resim")
        if rs is None:
            walk = "?"
        elif rs["dead"] == 0:
            walk = f"ok ({rs['of']}t)"
        else:
            walk = (f"DIES {rs['dead']}/{rs['of']}t @{rs['first']}"
                    f"{'' if rs['contig'] else '+'}")
        print(f"lv{lv} t={at} {note[:18]:<18} {r['verdict']:<8} {deep:<22} "
              f"{gd_tag:<6} {str(r.get('seconds', '-')):<6} {walk}{skip_why}")
        b = base.get(key, "")
        # a case that was SOLVED and is no longer = a regression. The reverse
        # is welcome.
        if b.startswith("SOLVED") and r["verdict"] != "SOLVED":
            bad.append(f"{key}: {b} -> {r['verdict']}")
        # GD survival is treated the same: OK turning into DEAD is a regression
        if "/OK" in b and r.get("gd") == "DEAD":
            bad.append(f"{key}: GD OK -> DEAD (t={r.get('gd_last')} "
                       f"< {r.get('gd_target')})")
    print(f"--- {len(cases)} cases {round(time.time() - t0, 1)}s ---")
    if a.bless:
        base_path.parent.mkdir(parents=True, exist_ok=True)
        base_path.write_text(json.dumps(now, indent=1))
        print(f"baseline updated: {base_path}")
        return 0
    if bad:
        print("FAIL:")
        for b in bad:
            print(f"  {b}")
        return 1
    if base:
        print("PASS (every case that was SOLVED still is)")
    else:
        print("no baseline yet (--bless creates one)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
