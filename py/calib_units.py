# -*- coding: utf-8 -*-
u"""COMPARE A CALIBRATION RIG AS A UNIT-BY-UNIT MATRIX (successor to calib_diff).

calib_diff only reports "the first divergence" for the rig as a whole, so on a
48-unit rig a broken first unit hides the remaining 47 (reproducing, on the rig
side, the same trap as census' "the family count does not move but its contents
are swapped out"). Here the ticks are sliced by the x bands in .units.json and
printed as a table with ONE UNIT PER ROW.

  python py/calib_units.py <rig name> [--exe <leveldp>] [--worker 99]
                           [--skip-build] [--skip-gd] [--tol 0.02]

Columns of the output: unit number / parameters / tick count / first divergent
tick / max |dy| / max |dvy| / verdict (OK / DIVERGE / EMPTY).
EMPTY = there is no common tick in that x band (GD died earlier, or the rig is
broken).

The GD-side artefacts are kept under data/ so they can be reused:
  data/objrects_calib_<rig>.txt   (model input)
  data/calib_<rig>.dump.csv       (GD's measurement)
--skip-gd just reads those (used for A/B on the model side).
"""
import argparse
import csv
import io
import json
import shutil
import subprocess
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from gdtas.paths import LAB_RIGS, LEVELDP_EXE, RIGS, WORKERS_ROOT  # noqa: E402

PY = sys.executable
SCRATCH = Path(__file__).resolve().parent.parent / "data" / "tmp_calib"


def load(path, stop_on_dead=False):
    u"""Read both dump.csv (GD, `yvel`) and the model's trace (`vy`)."""
    out = {}
    dead_at = None
    with io.open(path, newline='', encoding='utf-8-sig',
                 errors='replace') as f:
        for r in csv.DictReader(f):
            try:
                if stop_on_dead and int(r.get('dead', '0') or '0'):
                    dead_at = int(r['tick'])
                    break
                vy = r.get('yvel')
                if vy is None:
                    vy = r.get('vy', '0')
                out[int(r['tick'])] = (float(r['x']), float(r['y']),
                                       float(vy))
            except (ValueError, KeyError, TypeError):
                pass
    return out, dead_at


def label_of(u: dict) -> str:
    skip = {"x0", "x1", "pad", "edge", "xs", "w", "sy0", "ptop", "x_port"}
    parts = []
    for k, v in u.items():
        if k in skip:
            continue
        if isinstance(v, float) and v == int(v):
            v = int(v)
        parts.append(f"{k}={v}")
    return " ".join(parts)


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("rig")
    ap.add_argument("--worker", type=int, default=99)
    ap.add_argument("--exe", default=str(LEVELDP_EXE))
    ap.add_argument("--skip-build", action="store_true")
    ap.add_argument("--skip-gd", action="store_true",
                    help="use the dump/objrects left in data/ (for an A/B of the model)")
    ap.add_argument("--tol", type=float, default=0.02)
    ap.add_argument("--tolv", type=float, default=0.02)
    ap.add_argument("--show", type=int, default=3,
                    help="ticks to show either side of the divergence in a "
                         "DIVERGE unit")
    ap.add_argument("--settle", type=float, default=0.0,
                    help="leave the first N px of each unit out of the scoring, "
                         "to keep the known quirks of a unit boundary -- the one "
                         "tick a size change takes, for instance -- from "
                         "contaminating it")
    a = ap.parse_args()

    # The rig (.lvl / .units.json / .plan.txt) lives in the public data/rigs;
    # GD's measurements go to lab/rigs
    lvl = RIGS / f"calib_{a.rig}.lvl"
    units_p = lvl.with_suffix(".units.json")
    LAB_RIGS.mkdir(parents=True, exist_ok=True)
    dump_p = LAB_RIGS / f"calib_{a.rig}.dump.csv"
    objr = LAB_RIGS / f"objrects_calib_{a.rig}.txt"

    if not a.skip_build and not a.skip_gd:
        subprocess.run([PY, str(Path(__file__).resolve().parent / "mklevel.py"), a.rig,
                        "--out", str(lvl)], check=True)
    if not a.skip_gd:
        subprocess.run([PY, str(Path(__file__).resolve().parent / "run_calib.py"), str(lvl),
                        str(a.worker), "dump=1", "clearance=1"], check=True)
        wd = WORKERS_ROOT / f"worker-{a.worker}" / "session" / "data"
        shutil.copy2(wd / "dump.csv", dump_p)
        # Drop the spike artefact at the spawn point (id8 @ x=0, placed by the
        # worker). GD does not die on it thanks to spawn protection (this is
        # what the deaths p1=13 of every session really is), but to the model it
        # looks like a genuine hazard. Only the 0.7 rig died because its speed
        # portal fires at t~10 and lengthens the stay inside the kill window
        # |x|<18.
        lines = []
        for ln in io.open(wd / "objrects.txt", encoding="utf-8",
                          errors="replace"):
            p = ln.split(",")
            try:
                if len(p) > 3 and p[1] == "2" and float(p[2]) < 30.0:
                    continue
            except ValueError:
                pass
            lines.append(ln)
        io.open(objr, "w", encoding="utf-8", newline="").write("".join(lines))

    units = json.loads(units_p.read_text(encoding="utf-8"))
    gd, dead_at = load(dump_p, stop_on_dead=True)
    if dead_at is not None:
        dx0 = gd[max(gd)] if gd else (0, 0, 0)
        print(f"** GD died: t={dead_at} last x={dx0[0]:.1f} -- the rig is "
              f"supposed to be non-lethal. Every later unit comes out EMPTY")

    # Model replay
    plan = lvl.with_suffix(".plan.txt")
    if not plan.exists():
        plan = RIGS / "calib_empty_plan.txt"
        plan.write_text("input=0,0\n", encoding="utf-8", newline="")
    SCRATCH.mkdir(parents=True, exist_ok=True)
    base = SCRATCH / f"calibu_{a.rig}"
    trace = Path(str(base) + ".trace.csv")
    if trace.exists():
        trace.unlink()
    r = subprocess.run([a.exe, str(objr), "--replay", str(plan),
                        "--out", str(base)],
                       cwd=r"C:\GD", capture_output=True, text=True)
    if not trace.exists():
        print("the model produced no trace:")
        print((r.stdout or "")[-800:])
        return 2
    md, _ = load(trace)

    common = sorted(set(gd) & set(md))
    if not common:
        print("no tick is common to both")
        return 2

    # tick -> unit (looked up by GD's x)
    print(f"{a.rig}: {len(common)} ticks in common / GD {len(gd)} / model {len(md)}"
          f" / exe={Path(a.exe).name}")
    hdr = (f"{'#':>3} {'params':<38} {'ticks':>6} {'t_bad':>7} "
           f"{'max|dy|':>9} {'max|dvy|':>9}  verdict")
    print(hdr)
    print("-" * len(hdr))
    n_ok = n_div = n_empty = 0
    diverged = []
    for i, u in enumerate(units):
        T = [t for t in common
             if u["x0"] + a.settle <= gd[t][0] < u["x1"]]
        if not T:
            n_empty += 1
            print(f"{i:>3} {label_of(u):<38} {0:>6} {'-':>7} {'-':>9} "
                  f"{'-':>9}  EMPTY")
            continue
        first_bad = None
        wdy = wdvy = 0.0
        for t in T:
            dy = md[t][1] - gd[t][1]
            dvy = md[t][2] - gd[t][2]
            if abs(dy) > abs(wdy):
                wdy = dy
            if abs(dvy) > abs(wdvy):
                wdvy = dvy
            if first_bad is None and (abs(dy) > a.tol or abs(dvy) > a.tolv):
                first_bad = t
        if first_bad is None:
            n_ok += 1
            print(f"{i:>3} {label_of(u):<38} {len(T):>6} {'-':>7} "
                  f"{abs(wdy):>9.3f} {abs(wdvy):>9.3f}  OK")
        else:
            n_div += 1
            diverged.append((i, u, first_bad, T))
            print(f"{i:>3} {label_of(u):<38} {len(T):>6} {first_bad:>7} "
                  f"{abs(wdy):>9.3f} {abs(wdvy):>9.3f}  DIVERGE")
    print(f"\nOK {n_ok} / DIVERGE {n_div} / EMPTY {n_empty} "
          f"({len(units)} units in all)")

    for i, u, fb, T in diverged:
        print(f"\n-- unit {i} ({label_of(u)}) first divergence t={fb}")
        for t in range(fb - a.show, fb + a.show + 1):
            if t in gd and t in md:
                print(f"   t={t:<7} x={gd[t][0]:<10.3f} "
                      f"GD y={gd[t][1]:<9.3f} vy={gd[t][2]:<8.3f} "
                      f"M y={md[t][1]:<9.3f} vy={md[t][2]:<8.3f} "
                      f"dy={md[t][1] - gd[t][1]:+.3f} "
                      f"dvy={md[t][2] - gd[t][2]:+.3f}")
    return 1 if n_div else 0


if __name__ == "__main__":
    raise SystemExit(main())
