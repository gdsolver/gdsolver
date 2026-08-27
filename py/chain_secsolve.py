"""Solve a level to the end using only GD's section solver (chained sections).

No approximate model: it walks x forward using GD ITSELF AS THE TRANSITION
FUNCTION. Each section only hands the input sequence it produced to the next
section as a prefix, so no external ground-truth data is needed at all
(this satisfies the cold rule).

  python chain_secsolve.py --level 20 --worker 99 --step 3000

Every section is always verified by a plain replay on the MOD side; if it does
not pass, the verdict never becomes SOLVED (UNVERIFIED). On top of that, we
check the seam here every time with A PLAIN REPLAY FROM THE START OF THE LEVEL —
because the MOD's verification only ever speaks about "from the section CP".

If a section comes back EXHAUSTED (search failure), MOVE THE ENTRANCE BACKWARD
and solve again. It means "no sequence of presses gets through from that
entrance" = the entrance itself is already a dead end, so widening the cap does
not help (measured, docs/HANDOFF.md update 42).
"""
from __future__ import annotations
import argparse
import re
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from gdtas import plan as P                                # noqa: E402
from gdtas.paths import WORKERS_ROOT                       # noqa: E402
from gdtas.worker import run_session                       # noqa: E402

BASE = ["attempts=1", "quitwhendone=1", "blockinput=1", "cbs=0", "cos=1",
        "fastdt=0.0166667", "fastloops=1", "skiprender=1", "solve=0",
        "music=mute", "coins=0", "notrace=1"]


def log(m: str) -> None:
    print(f"[{time.strftime('%H:%M:%S')}] {m}", flush=True)


def solve(a, tick: int, target: float, horizon: int,
          inputs: list[tuple[int, int]], snap: int = 2) -> dict:
    cfg = ["enabled=1", f"level={a.level}"] + BASE + [
        f"practiceat={max(1, tick - 100)}", f"checkpointat={tick}",
        "secsolve=1", "seclog=1", f"secstart={tick}",
        f"sectarget={target:.3f}", f"sechorizon={horizon}",
        # LOWER THE CAP FOR CP. The cost grows superlinearly in the cap
        # (measured lv20 t=6060: 0.33 s/layer at cap 40, 8 s/layer at cap 200).
        # The same wall takes 8.6 min at cap 60 and 56 min at cap 200, and THE
        # RESULT IS IDENTICAL (both x=10021.4 t=6692, verification OK). CP also
        # grows to 12GB of memory and slows itself down, so widening is pure loss.
        f"seccap={a.cap if snap else a.cp_cap}", f"secsnap={snap}"]
    if snap:
        cfg.append("secverifyevery=20")
    cfg += [f"input={t},{d}" for t, d in inputs]
    res = run_session(a.worker, cfg, timeout_s=7200, workers_root=WORKERS_ROOT)
    out = {"verdict": "", "base": -1, "tick": -1, "x": 0.0, "seq": []}
    for ln in res.lines:
        m = re.search(r"secsolve: (\w+) .*?inputBase=(-?\d+).*?"
                      r"foundX=([-\d.]+) foundTick=(-?\d+)", ln)
        if m:
            out.update(verdict=m.group(1), base=int(m.group(2)),
                       x=float(m.group(3)), tick=int(m.group(4)))
        m = re.search(r"secsolve_inputs: ([01,]+)", ln)
        if m:
            out["seq"] = [int(c) for c in m.group(1).split(",") if c]
        if "secsolve_verify:" in ln:
            log("    " + ln.strip()[ln.find("secsolve_verify:"):][:110])
    return out


def splice(inputs: list[tuple[int, int]], base: int,
           seq: list[int]) -> list[tuple[int, int]]:
    """The section splice, lifted verbatim from the driver before it was deleted.

    The input at depth i (0-based) is absolute tick base+i. Of the prefix, keep
    only t < base. Emit presses only at change points. Back when this was
    base+1+i, a solution that passed the MOD's verification died at t=1654 on a
    plain replay.
    """
    kept = [(t, d) for t, d in inputs if t < base]
    held = kept[-1][1] if kept else 0
    for i, v in enumerate(seq):
        if v != held:
            kept.append((base + i, v))
            held = v
    return kept


def seam_ok(a, inputs: list[tuple[int, int]], need: int) -> int:
    """Plain replay from the start of the level; return the tick of death (-1 = survived)."""
    cfg = ["enabled=1", f"level={a.level}"] + BASE
    cfg += [f"input={t},{d}" for t, d in inputs]
    r = run_session(a.worker, cfg, timeout_s=1800, workers_root=WORKERS_ROOT)
    for ln in r.lines:
        m = re.search(r"death: attempt=\d+ tick=(\d+)", ln)
        if m:
            return int(m.group(1))
    return -1


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--level", type=int, default=20)
    ap.add_argument("--worker", type=int, default=99)
    ap.add_argument("--out", default=str(DATA / "solution_lv20_chain.txt"))
    ap.add_argument("--step", type=float, default=3000.0,
                    help="px to advance per section")
    ap.add_argument("--horizon", type=int, default=3000)
    ap.add_argument("--cap", type=int, default=200)
    ap.add_argument("--max-x", type=float, default=35635.0)
    ap.add_argument("--start-tick", type=int, default=200)
    ap.add_argument("--max-chunks", type=int, default=80)
    # How many ticks to move the entrance back on EXHAUSTED (tried in order)
    ap.add_argument("--backs", nargs="*", type=int, default=[800, 2000, 4000])
    # How far to extend when falling back to CP (as a fraction of step). CP is
    # expensive, so only cover the wall.
    ap.add_argument("--cp-frac", type=float, default=0.34)
    ap.add_argument("--cp-cap", type=int, default=60)
    # Resume from our own progress (.state.json). No external ground-truth data.
    ap.add_argument("--resume", action="store_true")
    a = ap.parse_args()

    out = Path(a.out)
    inputs: list[tuple[int, int]] = []
    tick, x = a.start_tick, 0.0
    t0 = time.time()
    state = Path(str(out) + ".state.json")
    if a.resume and state.exists() and out.exists():
        import json
        # Read as utf-8-sig: PowerShell's Out-File adds a BOM
        s = json.loads(state.read_text(encoding="utf-8-sig"))
        tick, x = int(s["tick"]), float(s["x"])
        inputs = [(t, d) for t, d in P.read_inputs(out)]
        log(f"resume: t={tick} x={x:.0f} ({len(inputs)} inputs) from {out}")

    for chunk in range(a.max_chunks):
        target = min(a.max_x + 60.0, x + a.step)
        got = None
        # The ladder. ON EXHAUSTED, TRY CP BEFORE MOVING THE ENTRANCE BACK.
        #
        # In some sections psnap makes the whole frontier a lie. Measured (the
        # lv20 x~9,550 wall):
        #   psnap+verify  at d=300, 179 of 200 were off by up to 6.5px; at d=340
        #                 all 200/200 died on replay -> frontier wiped, EXHAUSTED
        #   CP            same entrance, same cap: reached d=371 x=9,601 with dead=0
        # Verification does correctly expose this lie, but once exposed the only
        # way on is "redo it with CP". Moving the entrance back is the last
        # resort, for when even CP does not get through.
        for back, snap in ([(0, 2), (0, 0)]
                           + [(b, 0) for b in a.backs]):
            st = max(a.start_tick, tick - back)
            if back and st >= tick:
                continue
            hz = a.horizon + back
            how = "psnap+check" if snap else "CP"
            # SHORTEN THE TARGET FOR CP. CP takes 1-7 s per layer and grows to
            # 12GB of memory, slowing itself down. Advance only far enough to
            # clear the wall, then go back to psnap beyond it (walls are
            # localised per section).
            tg = target if snap else min(target, x + a.step * a.cp_frac)
            log(f"chunk {chunk}: t={st} x={x:.0f} -> {tg:.0f} "
                f"({how}, horizon {hz}"
                + (f", entry moved back {back} ticks" if back else "")
                + f", {len(inputs)} inputs)")
            r = solve(a, st, tg, hz, inputs, snap=snap)
            if r["verdict"] == "SOLVED" and r["seq"]:
                got = r
                break
            log(f"    {r['verdict'] or 'no verdict'}")
        if got is None:
            log(f"giving up (x={x:.0f})")
            break
        inputs = splice(inputs, got["base"], got["seq"])
        out.write_text(P.format_plan(inputs), encoding="utf-8")
        dtick = seam_ok(a, inputs, got["tick"])
        if 0 <= dtick < got["tick"]:
            log(f"  **the seam is broken**: a plain replay dies at t={dtick} "
                f"(the section exits at t={got['tick']}) - giving up")
            break
        tick, x = got["tick"], got["x"]
        import json
        state.write_text(json.dumps({"tick": tick, "x": x}), encoding="utf-8")
        el = int(time.time() - t0)
        log(f"  SOLVED -> t={tick} x={x:.0f} ({x / a.max_x * 100:.1f}%), "
            f"seam t={dtick if dtick >= 0 else 'clear'}, "
            f"{el // 60}m{el % 60:02d}s -> {out}")
        if x >= a.max_x:
            log("*** reached the end of the level ***")
            break


if __name__ == "__main__":
    main()
