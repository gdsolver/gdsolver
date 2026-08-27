# -*- coding: utf-8 -*-
"""Extract MEASURED SLOPE-EXIT LAUNCH VALUES from a calibration rig run.

The rig is built to be cleared with zero input (build_ramps in py/mklevel.py),
so every positive vy spike is a slope-exit launch. That removes any guessing
about "which one is the launch" -- this is exactly why the rig was designed to
need no input.

    python py/calib_extract.py --dump <dump.csv> --units data/calib_ramps.units.json

What comes out: the launch vy per (mode, |m|, size, number of ramps), together
with the preceding ride length in ticks. In a form that plugs straight into
leveldp's slopeExitVy / slopeRampFactor.
"""
from __future__ import annotations

import argparse
import csv
import json
from pathlib import Path

# GD's x increment (1x speed). Used to estimate ride ticks from px
DX_1X = 1.29825044


def main_orb(a) -> int:
    """Orb rig: THE TICK WHERE vy JUMPS WHILE FALLING is the fire.

    The rig is built so the press only ever happens in mid-air (orb_unit in
    py/mklevel.py), so grounded jumps never get mixed in. It prints the vy at
    contact (= the incoming vy) next to the vy after the fire, so it maps
    directly onto the model's per-orb / per-mode constants.
    """
    units = json.loads(Path(a.units).read_text(encoding="utf-8"))
    rows = []
    with Path(a.dump).open(newline="", encoding="utf-8-sig",
                           errors="replace") as f:
        for r in csv.DictReader(f):
            try:
                rows.append((int(r["tick"]), float(r["x"]), float(r["y"]),
                             float(r["yvel"]), r["mode"],
                             float(r.get("vsize") or 1),
                             int(r["onGround"]),
                             int(float(r.get("upsideDown") or 0))))
            except (ValueError, KeyError):
                continue

    def unit_of(x: float) -> dict | None:
        for u in units:
            if u["x0"] <= x < u["x1"]:
                return u
        return None

    print(f"{'mode':<7} {'orb':<9} {'size':<5} {'vy in':>8} {'after':>8} "
          f"{'ratio':>6} {'flip':>4} {'x':>8}")
    # Index by tick (so we can look only inside the press window)
    by_t = {r[0]: i for i, r in enumerate(rows)}
    out, seen = [], set()
    for u in units:
        # Scan INSIDE THE PRESS WINDOW ONLY. Taking the first spike within the
        # unit's x band would misread the landing of the previous unit's arc as
        # a fire.
        hit = None
        for t in range(u.get("t_press", 0), u.get("t_rel", 0) + 3):
            i = by_t.get(t)
            if not i or i < 1:
                continue
            vy, pvy = rows[i][3], rows[i - 1][3]
            if abs(vy - pvy) >= a.min_jump:
                hit = i
                break
        if hit is None:
            continue
        i = hit
        t, x, y, vy, mode, vs, og, up = rows[i]
        pvy = rows[i - 1][3]
        if u["x0"] in seen:
            continue
        seen.add(u["x0"])
        size = "mini" if vs < 0.9 else "full"
        ratio = (vy / pvy) if abs(pvy) > 1e-6 else float("nan")
        print(f"{mode:<7} {u['orb']:<9} {size:<5} {pvy:>8.3f} {vy:>8.3f} "
              f"{ratio:>6.3f} {up:>4} {x:>8.0f}")
        out.append({"mode": mode, "orb": u["orb"], "mini": u["mini"],
                    "vy_in": round(pvy, 4), "vy_out": round(vy, 4),
                    "flip": up, "x": round(x, 1)})
    print(f"\n{len(out)} firings / {len(units)} units")
    return 0


def main_orbedge(a) -> int:
    """Sweep of the fire boundary: per unit, report "did it fire" and the real offset.

    The press lasts 1 tick, so on a fire you get the orb's value (11.180 for
    yellow), and otherwise the player's own action (a grounded cube jump is also
    11.180, the same value, so LOOK AT y TOO: an orb fires in mid-air as well,
    but here both are grounded, so no distinction is needed).
    Because the press is 1 tick, both a jump and an orb make vy spike. SO "DID
    IT SPIKE" DOES NOT GIVE YOU THE BOUNDARY. The rig uses the pink orb (8.050),
    which can be told apart from the cube jump (11.180) by value:
    1.0 < vy < 10.0 means it fired.
    """
    units = json.loads(Path(a.units).read_text(encoding="utf-8"))
    rows = {}
    with Path(a.dump).open(newline="", encoding="utf-8-sig",
                           errors="replace") as f:
        for r in csv.DictReader(f):
            try:
                rows[int(r["tick"])] = (float(r["x"]), float(r["y"]),
                                        float(r["yvel"]),
                                        float(r.get("vsize") or 1),
                                        int(r["onGround"]))
            except (ValueError, KeyError):
                continue
    print(f"{'mini':>4} {'delta':>6} {'player x':>10} {'orb x':>10} "
          f"{'gap':>7} {'vy':>8} {'fired?':>6}")
    prev_key, boundary = None, {}
    for u in units:
        t = u["t_press"]
        if t not in rows or (t + 1) not in rows:
            continue
        px = rows[t][0]
        vy_after = max(rows.get(t + k, (0, 0, 0, 0, 0))[2] for k in (1, 2))
        # TAKE THE ORB'S TRUE x FROM THE UNIT TABLE. Using delta directly as the
        # gap puts tick_at's rounding (up to 1.3px) into the boundary.
        orb_x = u.get("x_orb", px + u["delta"])
        # Pink is 8.050 / 6.440 mini, against the player's own jump 11.180 / 8.944.
        # DECIDE BY THE EXPECTED VALUE FOR EACH SIZE -- judging mini with
        # 1.0<vy<10.0 makes the mini jump 8.944 look like a fire and every unit
        # comes out yes.
        if "own" in u:
            # Mode-sweep rig: decide by EXCLUDING THE VALUE OF THE PLAYER'S OWN
            # ACTION (spider's teleport +/-1.000, robot's jump 5.590/4.472). If
            # the orb fires the value is different, so we do not need to know
            # the expected value for each mode.
            fired = (abs(abs(vy_after) - u["own"]) > 0.3
                     and abs(vy_after) > 0.5)
        else:
            want = 6.440 if u["mini"] else 8.050
            fired = abs(vy_after - want) < 0.2
        gap = orb_x - px
        print(f"{u['mini']:>4} {u['delta']:>6} {px:>10.3f} {orb_x:>10.3f} "
              f"{gap:>7.2f} {vy_after:>8.3f} {'yes' if fired else 'no':>6}")
        # Rigs with a dy (orbedge2) report per dy; the mode sweep reports per mode
        key = (u["mini"], u.get("dy", 0.0), u.get("mode", "cube"))
        boundary.setdefault(key, []).append((gap, fired))
    print()
    for k in sorted(boundary):
        v = boundary[k]
        fires = [g for g, f in v if f]
        miss = [g for g, f in v if not f]
        tag = f"{k[2]:<6} " + ("mini" if k[0] else "full") + f" dy={k[1]:+.0f}"
        if not fires:
            print(f"{tag}: never fired ({len(v)} units)")
            continue
        lo, hi = min(fires), max(fires)
        above = min((g for g in miss if g > hi), default=None)
        # Clamped distance (for the circle-vs-box hypothesis): what is left after
        # subtracting the player half-width, and what is left on the dy side
        half = 9.0 if k[0] else 15.0
        cx_ = max(0.0, abs(hi) - half)
        cy_ = max(0.0, abs(k[1]) - half)
        clamped = (cx_ ** 2 + cy_ ** 2) ** 0.5
        print(f"{tag}: fires over gap {lo:+.2f} .. {hi:+.2f} "
              f"({len(fires)}/{len(v)})"
              f"  next miss {('-' if above is None else round(above, 2))}"
              f"  clamped(farthest firing)={clamped:.2f}")
    return 0


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--dump", required=True)
    ap.add_argument("--units", required=True)
    ap.add_argument("--min-jump", type=float, default=1.0,
                    help="the one-tick rise in vy that counts as a launch")
    ap.add_argument("--kind", choices=("ramp", "orb", "orbedge"),
                    default="ramp",
                    help="orb: the tick where a tap makes vy jump / "
                         "orbedge: the firing boundary")
    a = ap.parse_args()
    if a.kind == "orb":
        return main_orb(a)
    if a.kind == "orbedge":
        return main_orbedge(a)
    units = json.loads(Path(a.units).read_text(encoding="utf-8"))

    rows = []
    with Path(a.dump).open(newline="", encoding="utf-8-sig",
                           errors="replace") as f:
        for r in csv.DictReader(f):
            try:
                rows.append((int(r["tick"]), float(r["x"]), float(r["y"]),
                             float(r["yvel"]), r["mode"],
                             float(r.get("vsize") or 1), int(r["onGround"])))
            except (ValueError, KeyError):
                continue

    def unit_of(x: float) -> dict | None:
        for u in units:
            if u["x0"] <= x < u["x1"]:
                return u
        return None

    print(f"{'mode':<7} {'|m|':>4} {'size':<5} {'n':>2} {'launch vy':>9} "
          f"{'ride tick':>9} {'x':>9}")
    out = []
    for i in range(1, len(rows)):
        t, x, y, vy, mode, vs, og = rows[i]
        pvy = rows[i - 1][3]
        # A launch is "jumping to a large positive vy". A landing (negative -> 0)
        # also has a positive delta, so REQUIRE THE RESULTING vy ITSELF TO BE
        # POSITIVE (without this, landings get mixed in as launches and 0.000
        # values line up under the same condition).
        if vy - pvy < a.min_jump or vy <= a.min_jump:
            continue
        u = unit_of(x)
        if not u:
            continue
        # Ride ticks: the length of the run where y increased monotonically
        # (i.e. while climbing the ramp)
        j, ride = i - 1, 0
        while j > 1 and rows[j][2] > rows[j - 1][2] + 1e-6:
            ride += 1
            j -= 1
        size = "mini" if vs < 0.9 else "full"
        print(f"{mode:<7} {u['m']:>4} {size:<5} {u['ramps']:>2} "
              f"{vy:>9.3f} {ride:>8d} {x:>9.0f}")
        out.append({"mode": mode, "m": u["m"], "mini": u["mini"],
                    "ramps": u["ramps"], "vy": round(vy, 4), "ride": ride,
                    "x": round(x, 2)})
    print(f"\n{len(out)} launches / {len(units)} units")
    # If the same condition occurs more than once, check they agree (rig
    # reproducibility check)
    seen: dict[tuple, list[float]] = {}
    for o in out:
        seen.setdefault((o["mode"], o["m"], o["mini"], o["ramps"]),
                        []).append(o["vy"])
    dup = {k: v for k, v in seen.items() if len(v) > 1}
    if dup:
        print("duplicates under the same conditions "
              "(values that disagree mean the rig or the extraction is wrong):")
        for k, v in dup.items():
            print(f"  {k}: {v}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
