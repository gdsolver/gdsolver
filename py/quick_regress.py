"""A regression that runs in seconds. NO GD AND NO WORKER NEEDED.

    python py/quick_regress.py                 # verdict (~5 s)
    python py/quick_regress.py --record        # re-record the GD reference (slow)
    python py/quick_regress.py --bless         # adopt the current numbers as baseline

## Why it was built

The full regression (`cold_regress.py`) is the only tool that measures
"does it keep solving cold", but it takes 30 minutes. RUNNING IT EVERY TIME
IN THE MIDDLE OF SOLVING IS NOT A SENSIBLE USE OF TIME (user policy
2026-08-10: full regression only when solving is done).

Meanwhile, most of what you want to know after touching the model is "CAN THE
MODEL STILL REPLAY A VERIFIED SOLUTION THE WAY GD DOES", and that can be
measured without re-solving:

  - a GD replay is deterministic for the same input -> THE GD SIDE CAN BE
    RECORDED ONCE AND REUSED
  - a model replay takes 0.1-0.2 s -> a few seconds even for 22 levels

So the reference (GD's dump) is kept in `data/gdref/`, and from then on only
the model is run and compared. It uses no worker, so IT CAN RUN ALONGSIDE A
LONG SOLVING RUN.

`data/gdref/` is not in git (30MB, and it can be regenerated):

    python py/quick_regress.py --record   # record the GD reference (33 s, 6 workers)
    python py/quick_regress.py --bless    # adopt the current numbers as baseline (81 s)

RE-RECORD THE REFERENCE WHENEVER YOU REPLACE A SOLUTION. The reference is "the
trajectory GD produced when replaying that solution", so once the solution
changes the reference is a different thing.

## How it measures -- SECTIONS, not the whole run

In a single whole-run replay everything downstream of the first divergence
loses its meaning, and the model usually dies early (lv19 at t=4,711). 80% OF
THE LEVEL CANNOT BE MEASURED. In fact the whole-run version returned exactly
the same numbers for the build carrying the "accumulated lag" bug and for the
fixed one = zero detection power.

So GD's real state is passed in `--start` and the model is run for a short
stretch (the same as the driver's re-anchor), and only that window is looked
at. The metric is not "the number of diverging ticks" but
THE NUMBER OF TICKS BEFORE THE FIRST DIVERGENCE (tracking) -- the former
saturates at the window length. leveldp is deterministic, so
BIT-IDENTICAL PER SECTION FOR THE SAME BUILD is the expectation. That is why a
drop of even 1 tick is reported (`--seg-slack` loosens it).

Detection power changes with density (measured on lv20, with "accumulated lag +
trigclosed OFF" applied together):

| --seg-step | all 20 levels | detection |
|---|---|---|
| 1500 | 20 s | lv19 shrinks by 1 tick, and only barely |
| **400 (default)** | **81 s** | lv20 t=22,200 collapses from 400 to 16 ticks |

## What it catches and what it does not

Catches: regressions in the fidelity of physics, geometry and moving geometry.
THAT IS MOST OF WHAT MAKES A LEVEL STOP BEING SOLVABLE.

Does not catch: regressions purely on the search side (cap, dedupe, ladder,
granularity) -- the kind where the trajectory is the same but the frontier is
carried differently. THAT ONLY COMES OUT IN THE FULL REGRESSION.

## The three tiers (user policy 2026-08-10)

| | what | time | when |
|---|---|---|---|
| 1 | `quick_regress.py` | **81 s**, no worker | on every model change; runs beside a long solve |
| 2 | `cold_regress.py --levels <a few>` | minutes, 4 workers | before committing a change |
| 3 | `cold_regress.py` (all 22) | ~30 min | **ONLY WHEN A LEVEL IS SOLVED** |
"""

from __future__ import annotations

import argparse
import csv
import json
import subprocess
import sys
import time
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

_PY = Path(__file__).resolve().parent
sys.path.insert(0, str(_PY))
sys.path.insert(0, str(_PY.parent / "mcp"))

from gdtas.solveutil import (has_grouped_colliders, grounded_of, held_before,
                             FLYING, MODE_ID)
from fidelity_diff import groups_args, model_replay, gd_cut_tick, gd_replay
from gdmcp.data import diff_trace
from gdtas.paths import DATA, LEVEL_DATA, LEVELDP_EXE, WORKERS_ROOT

REF = LEVEL_DATA / "gdref"
BASELINE = REF / "baseline.json"
# Only the columns the reference needs. The first half is what diff_trace
# reads, the second half is what building --start needs (the same columns
# an anchor is built from a GD dump row).
# The raw dump is 2.5MB on lv20; trimmed it comes to less than half that
REF_COLS = ["attempt", "tick", "x", "y", "yvel", "mode", "vsize",
            "dual", "p2y", "p2vy", "p2up", "p2ground", "p2ground2",
            "upsideDown", "onGround", "onGround2", "speed", "pmin", "pmax",
            "snapuid", "snapdist", "gframe", "ctrlOff", "rot"]


def ctrlwin_args(level: int) -> list[str]:
    """Build `--ctrlwin t0:t1,...` from the ctrlOff column of the GD reference.

    THE MODEL CANNOT DERIVE LOSS OF CONTROL (id 2899 / Options trigger) ON ITS
    OWN. Which x crossing makes GD raise it is unsolved (however many times the
    same x is crossed it happens only once, and not necessarily on the first
    crossing), and firing it on a guess does more harm than the real thing.
    Treated like gframe and pmin/pmax: only what GD knows is passed on.

    Measured on lv22 (2026-08-18): 4 windows, 1,921 ticks in total. During them
    GD ignores the button entirely, while the model alone was jumping
    (fixcensus's `m0/mini0/g1/gdg1/sp0.9/air/in1` = edvy -11.180).
    """
    gd = read_ref(level)
    wins, t0 = [], None
    for t in sorted(gd):
        on = gd[t].get("ctrlOff") == "1"
        if on and t0 is None:
            t0 = t
        elif not on and t0 is not None:
            wins.append((t0, t - 1))
            t0 = None
    if t0 is not None:
        wins.append((t0, max(gd)))
    if not wins:
        return []
    return ["--ctrlwin", ",".join(f"{a}:{b}" for a, b in wins)]


def trim_dump(src: Path, dst: Path) -> int:
    """Trim GD's dump down to the reference columns and write it. Returns the rows."""
    n = 0
    dst.parent.mkdir(parents=True, exist_ok=True)
    with src.open(newline="", encoding="utf-8-sig", errors="replace") as f, \
            dst.open("w", newline="", encoding="utf-8") as g:
        rd = csv.DictReader(f)
        w = csv.DictWriter(g, fieldnames=REF_COLS, extrasaction="ignore")
        w.writeheader()
        for rec in rd:
            w.writerow({k: rec.get(k, "") for k in REF_COLS})
            n += 1
    return n


def plan_of(level: int, pattern: str) -> Path:
    return Path(pattern.replace("{}", str(level)))


def read_ref(level: int) -> dict[int, dict]:
    ref = REF / f"lv{level}.csv"
    rows: dict[int, dict] = {}
    if not ref.exists():
        return rows
    with ref.open(newline="", encoding="utf-8-sig", errors="replace") as f:
        for rec in csv.DictReader(f):
            try:
                rows[int(rec["tick"])] = rec
            except (KeyError, ValueError, TypeError):
                continue
    return rows


def robot_hover_left(t: int, r: dict, ref: dict, held: int) -> int:
    u"""The robot hover budget left at the anchor tick (--start field 16, a[15]).

    The dump has no rHover, but hover shows up perfectly as "a run of constant
    vy": armed with 67 on the jump/ring tick, then decreasing by 1 each tick.
    The length n of the run of the same vy (airborne, robot, >0) going back from
    the anchor tick is what has been spent, so the remainder = 67 - n. FLATTEN
    THIS TO 0 AND AN ANCHOR TAKEN DURING A HOVER DECAYS IMMEDIATELY, WHICH GIVES
    THE FAMILY OF 4 CASES ACROSS lv20/21/22 WITH edvy=+0.194 (one step of robot
    gravity) (lv20 t=7,802: GD holds 5.59 until the release plus the delay,
    while the section decays at 7,801).
    Hover does not continue unless held, so 0 in that case (the gate side is
    closed by s.action as well)."""
    if MODE_ID.get(r.get("mode"), 0) != 5 or not held:
        return 0
    if r.get("onGround") == "1":
        return 0
    try:
        vy = float(r["yvel"])
    except (KeyError, ValueError, TypeError):
        return 0
    if vy <= 0:
        return 0
    n = 0
    while n < 80:
        p = ref.get(t - n - 1)
        if not p or MODE_ID.get(p.get("mode"), 0) != 5 \
                or p.get("onGround") == "1":
            break
        try:
            if abs(float(p["yvel"]) - vy) > 0.0005:
                break
        except (KeyError, ValueError, TypeError):
            break
        n += 1
    # n=0 means "the first of the run" -- only a jump tick (grounded on the
    # tick before) is the real thing. AN ARC ALREADY DECAYING ALSO GIVES n=0,
    # so without this check every airborne held robot arms with 67 and the tick
    # right after the anchor loses its gravity (the misfire that collapsed the
    # lv22 t=19,800 section from 400 to 7).
    if n == 0:
        p = ref.get(t - 1)
        if not p or p.get("onGround") != "1":
            return 0
    return max(0, 67 - n)


def start_fields(t: int, r: dict, plan: Path, prev: dict | None = None,
                 ref: dict | None = None) -> str:
    """Copy one GD reference row into leveldp's 22 `--start` fields.

    IT CARRIES dual AND THE ROTATION FRAME TOO. The original implementation had
    20 fields and skipped dual rows entirely, on the grounds that they could not
    serve as anchors. What those two holes meant was that "the 2-minute harness
    never measures the dual section of lv16 or the rotation section of lv22":
    measured 2026-08-17, a change that made the dual p2 rules act on a single
    player as well stayed PASS in quick_regress while breaking the rotation
    section of lv22 (stuck at t=6,010 for 8+ iterations) -- IT WAS ONLY VISIBLE
    IN A TWO-HOUR COLD RUN.

    The fields follow the order of leveldp's --start (a[0..21]):
      0 t / 1 x / 2 y / 3 vy / 4 mode / 5 grounded / 6 held / 7 flip / 8 mini
      9 dual / 10 y2 / 11 vy2 / 12 flip2 / 13 grounded2 / 14 speed
      15 rHover / 16 dashing / 17 dashSlope / 18 snapuid / 19 snapdist
      20 frame / 21 rev
    """
    mode = MODE_ID.get(r["mode"], 0)
    g = grounded_of(mode, r.get("onGround", "0"), r.get("onGround2", "0"),
                    r["yvel"])
    held = held_before(plan, t)
    fl = 1 if r.get("upsideDown") == "1" else 0
    mn = 1 if (r.get("vsize") and float(r["vsize"]) < 0.9) else 0
    sp = r.get("speed") or 0
    # fields 19/20 = the stair snap (uid of the object being ridden and
    # m_snapDistance). Without them the first stair after the anchor always
    # drops, and a 1px shift in the section gets counted as AN ARTEFACT OF THE
    # HARNESS (measured 2026-08-10: 64 sections). An old reference without the
    # columns gets -1/0 = the old behaviour.
    su = r.get("snapuid") if r.get("snapuid") not in (None, "") else -1
    sd = r.get("snapdist") if r.get("snapdist") not in (None, "") else 0
    # the second player. On an old reference without the columns this falls
    # back to dual=0, i.e. the old behaviour.
    du = 1 if r.get("dual") == "1" else 0
    y2 = (r.get("p2y") or 0) if du else 0
    vy2 = (r.get("p2vy") or 0) if du else 0
    f2 = (1 if r.get("p2up") == "1" else 0) if du else 0
    # p2ground gets the SAME treatment as p1's onGround. GD's ground flag is
    # sticky for the flying modes -- it stays 1 for hundreds of ticks after the
    # body has left the surface -- and the model's `grounded` means "resting",
    # which restarts the velocity from 0. grounded_of filters p1's with
    # |yvel| < 0.01; the second body was copied raw.
    # Measured on lv16 t=9,400 (dual ship, p2ground=1 with p2vy=+0.069): the
    # anchor grounded p2, the model's first step gave it 0.086 (one gravity step
    # from rest) where GD has 0.155 (0.069 + one step), and that ONE MISSING
    # STEP is the whole of the section's divergence -- p1 tracks GD to 0.0000
    # for 400 ticks while p2's error grows 0.0193 px a tick.
    # p2ground2 is the second body's partner flag. A reference recorded before
    # the column existed simply has no value, and the |p2vy| test alone carries
    # the rule (the mod's groundedOf2 has both flags to hand).
    g2b = r.get("p2ground2")
    g2 = (1 if (r.get("p2ground") == "1"
                and (g2b is None or g2b == "" or g2b == "1")
                and (mode not in FLYING or abs(float(vy2 or 0)) < 0.01))
          else 0) if du else 0
    # the rotation frame. Converts GD's gframe (0-3) into the model's
    # (frame, rev, flip). Passed raw it gives "the DP says SOLVED, GD dies in
    # the same place" (the same conversion as the note in mkstart. frame 2 =
    # (frame 0, rev 1); on frame 3 the vertical is mirrored).
    try:
        gf = int(float(r.get("gframe") or 0))
    except ValueError:
        gf = 0
    if gf == 2:
        frame, rev = 0, 1
    elif gf == 3:
        frame, rev = 3, 0
        fl = 1 - fl
    else:
        frame, rev = gf, 0
    # THERE IS BACKWARD MOTION THAT DOES NOT SHOW UP IN gframe (2026-08-18).
    # In lv22 t=18,4xx-18,7xx GD's x decreases every tick while the dump's
    # gframe stays 0 -- a different section from the backward motion caught by
    # gframe=2 (the 2900 pair). Anchoring with rev=0 makes the model run
    # forward, GD's floor vanishes from the near slice, and the landing is
    # missed entirely (this is what the lv22 m5/mini1 cluster in the census
    # really was). World x on frame 0 never goes backwards under forced
    # scrolling, so THE x TREND OF THE REFERENCE ITSELF IS THE TRUTH ABOUT rev:
    # if it decreased from the previous row, it is running backwards.
    if frame == 0 and rev == 0 and prev is not None:
        try:
            if float(r["x"]) - float(prev["x"]) < -0.1:
                rev = 1
        except (KeyError, ValueError, TypeError):
            pass
    # fields 23/24 = the player's sprite rotation and the direction it turns.
    #
    # THE HITBOX OF A ROTATED OBJECT IS DECIDED BY THIS ALONE. An anchor
    # without it rebuilds from rot=0, so mid-section it is tens of degrees away
    # from GD, and near 45 degrees the player's projection is at its maximum
    # (15*sqrt2 = 21.21) and IT BITES INTO A ROTATED BOX TOO EARLY.
    # Measured on lv20 t=7,281, the -46 degree teleport portal uid7021:
    #   GD does not fire at rot=-420.303 (14.3 degrees off the portal axis); it
    #   fires on the next tick. The model warped 1 tick early, 192px off.
    # The direction is deduced from the difference with the previous row (GD
    # flips the sign on every gravity reversal, so the value alone does not
    # settle it). With no previous row it is 0 = as before.
    rot = r.get("rot") or 0
    rotneg = 0
    if prev is not None and prev.get("rot") not in (None, ""):
        try:
            # rotNeg=1 means "turns in the negative direction" = the side where
            # the dump's rot decreases.
            rotneg = 1 if float(rot) < float(prev["rot"]) else 0
        except ValueError:
            rotneg = 0
    # field 16, rHover: no such column in the dump, but it can be derived from
    # "the length of the run of constant vy". Only callers that passed ref (all
    # the reference rows) get the benefit; without it, the old 0.
    rh = robot_hover_left(t, r, ref, held) if ref is not None else 0
    dsh, dsl = dash_at(t, r, ref, held, prev) if ref is not None else (0, 0)
    return (f"{t},{r['x']},{r['y']},{r['yvel']},{mode},{g},{held},{fl},{mn},"
            f"{du},{y2},{vy2},{f2},{g2},{sp},{rh},{dsh},{dsl},{su},{sd},"
            f"{frame},{rev},{rot},{rotneg}")


def dash_at(t: int, r: dict, ref: dict, held: int, prev: dict | None
            ) -> tuple[int, float]:
    u"""The dash state at the anchor tick (--start fields 17/18, a[16]/a[17]).

    There is no column in the dump, but a dash shows up perfectly in the form
    of VY STAYING EXACTLY 0 WHILE AIRBORNE (y moves by a constant amount each
    tick, by the angle of the ring). It only continues while the button is
    held, so held is needed too.

    FLATTEN THIS TO 0 AND AN ANCHOR TAKEN DURING A DASH TAKES GRAVITY FROM THE
    NEXT TICK ON. Measured on lv21 t=19,000: GD goes straight on to 19,006 at
    y=211.472 vy=0, while the section drops to vy=-0.216 at 19,001 and diverged
    from there on (the same hole as rHover).

    The test is "at least 2 ticks, airborne (onGround=0), with vy exactly 0".
    The freeze on the tick of a jump only lasts 1 tick, so it cannot get mixed
    in. The slope is the measured y difference / x difference, and rotation
    sections (gframe != 0) are skipped because the meaning of the coordinates
    is swapped there.
    """
    if not held or r.get("onGround") == "1" or prev is None:
        return (0, 0)
    try:
        if float(r["yvel"]) != 0.0 or float(prev["yvel"]) != 0.0:
            return (0, 0)
        if prev.get("onGround") == "1":
            return (0, 0)
        if int(float(r.get("gframe") or 0)) != 0:
            return (0, 0)
        dx = float(r["x"]) - float(prev["x"])
        if dx <= 0.0:
            return (0, 0)
        return (1, (float(r["y"]) - float(prev["y"])) / dx)
    except (KeyError, ValueError, TypeError):
        return (0, 0)


_ROT2900: dict[int, list] = {}


def rot_anchor_args(level: int, t0: int) -> list[str]:
    """Give a section anchor on a 2900-holding level the same world history as the driver.

    --spentrot: the uids of 2900 already fired before t0. A one-shot (firedT)
    cannot be carried in --start, and a bare section re-fires a SPENT trigger
    lying ahead of the anchor on the next crossing, rotating the whole world
    with it (lv22 t=14,349 uid6337: GD had spent it on the first forward pass
    and went straight through, while the model alone entered the vertical
    section and x jumped 541px).
    --trigraw: for the autonomous triggers behind, trust the tick in the
    recording (est's distance conversion breaks down behind the rotation maze).
    Both follow the same prescription as the loop's own anchor arguments, and
    levels without any 2900 (lv1-21) get an empty list = bit-identical to
    before."""
    if level not in _ROT2900:
        from gdtas.solveutil import rot2900s
        _ROT2900[level] = rot2900s(LEVEL_DATA / f"objrects_lv{level}.txt")
    rots = _ROT2900[level]
    if not rots:
        return []
    from gdtas.solveutil import spent2900_from_dump
    extra = ["--trigraw"]
    spent = spent2900_from_dump(REF / f"lv{level}.csv", -1, t0, rots)
    if spent:
        extra += ["--spentrot", ",".join(str(u) for u in spent)]
    return extra


_BANDTRACK: dict[int, list[str]] = {}


def band_track_args(level: int, gd: dict) -> list[str]:
    """Pass the rows where gdref's pmin/pmax change through --bandtrack.

    The loop always passes this in the
    production replay verification: GD's band moves every tick with the camera
    and the zoom, so the value frozen at the anchor becomes a lie mid-section
    (lv22 t=8,458: the search rectangle of the spider warp was cut by the
    model's stale band 600 and landed 60px short -- GD's real band was 755).
    If only the measurement rig omits it, divergences that do not exist in the
    deployed configuration get counted as families.
    --startband (the anchor row) stays as it is: it becomes the initial value
    for ticks with no recording."""
    if level in _BANDTRACK:
        return _BANDTRACK[level]
    out = REF / f"lv{level}.bandtrack.txt"
    args: list[str] = []
    try:
        rows = 0
        with open(out, "w", encoding="utf-8") as w:
            prev = None
            for t in sorted(gd):
                r = gd[t]
                cur = (r.get("pmin"), r.get("pmax"))
                if not cur[0] or not cur[1]:
                    continue
                if cur != prev:
                    w.write(f"{t},{cur[0]},{cur[1]}\n")
                    rows += 1
                prev = cur
        if rows:
            args = ["--bandtrack", str(out)]
    except OSError:
        pass
    _BANDTRACK[level] = args
    return args


def seg_jobs(level: int, a) -> tuple[dict, list]:
    """ONLY BUILDS the section jobs. Running them is the caller's business (we want
    to line every level's jobs up before feeding them to the pool). Running them
    serially within a level piles up waiting on levels like lv20 where groups is
    24MB."""
    out = {"level": level, "status": "OK", "note": "", "segs": 0, "bad": 0,
           "worst_t": None, "worst": 0.0}
    plan = plan_of(level, a.plans)
    gd = read_ref(level)
    if not plan.exists() or not gd:
        out["status"] = "SKIP"
        out["note"] = "no solution" if not plan.exists() else "no reference"
        return out, []
    objrects = LEVEL_DATA / f"objrects_lv{level}.txt"
    if has_grouped_colliders(objrects) and not groups_args(plan):
        out["status"], out["note"] = "SKIP", f"no {plan.name}.groups*.txt"
        return out, []
    cuts = json.loads((REF / "cut.json").read_text()) if (REF / "cut.json").exists() else {}
    cut = cuts.get(str(level)) or max(gd)
    tmp = Path(a.tmp)
    tmp.mkdir(parents=True, exist_ok=True)
    common = [str(objrects), "--replay", str(plan)]
    trig, grp = LEVEL_DATA / f"triggers_lv{level}.txt", LEVEL_DATA / f"objgroups_lv{level}.txt"
    if trig.exists() and grp.exists():
        common += ["--triggers", str(trig), "--objgroups", str(grp)]
    obb = LEVEL_DATA / f"obb_lv{level}.txt"
    if obb.exists():
        common += ["--obb", str(obb)]
    common += groups_args(plan)
    common += ctrlwin_args(level)

    jobs = []
    t = a.seg_start
    while t + a.seg_len <= cut:
        r = gd.get(t)
        if r:
            args = list(common) + [
                "--start", start_fields(t, r, plan, gd.get(t - 1), gd),
                "--out", str(tmp / f"seg_lv{level}_{t}")]
            if r.get("pmin") and r.get("pmax"):
                args += ["--startband", f"{r['pmin']},{r['pmax']}"]
            args += band_track_args(level, gd)
            args += rot_anchor_args(level, t)
            jobs.append({"level": level, "t0": t, "args": args,
                         "t1": min(t + a.seg_len, cut),
                         "trace": tmp / f"seg_lv{level}_{t}.trace.csv"})
        t += a.seg_step
    out["segs"] = len(jobs)
    return out, jobs


def run_seg(job: dict, a) -> tuple[int, int, int]:
    """Run one section and return (level, t0, THE NUMBER OF TICKS IT HELD OUT).

    What is measured is not "the number of diverging ticks" but "THE NUMBER OF
    TICKS UNTIL THE FIRST DIVERGENCE". The former saturates at the window
    length (measured: with a 400 tick window the worst cases all line up at
    400/400 and cannot get any worse = zero detection power). The latter does
    not saturate, and it is exactly the thing we want to know: how far the
    model can keep up with GD.
    """
    subprocess.run([a.leveldp] + job["args"], stdout=subprocess.DEVNULL,
                   stderr=subprocess.DEVNULL)
    d = diff_trace(job["trace"], REF / f"lv{job['level']}.csv", t0=job["t0"],
                   t1=job["t1"], tol=a.tol, limit=10 ** 9)
    span = job["t1"] - job["t0"]
    if "error" in d or not d["rows"]:
        return job["level"], job["t0"], span
    return job["level"], job["t0"], int(d["rows"][0][0]) - job["t0"]


def seg_check(level: int, a) -> dict:
    """ANCHOR EACH SECTION FROM GD'S REAL STATE and look only at local agreement.

    In a single whole-run replay everything downstream of the first divergence
    loses its meaning and the model usually dies early (lv19 at t=4,711). That
    way 80% OF THE LEVEL CANNOT BE MEASURED -- in fact the whole-run version
    could not tell the build carrying the "accumulated lag" bug from the fixed
    one (both returned exactly the same numbers).

    So we do the same thing as the driver's re-anchor, at even intervals: pass
    GD's real state in `--start`, run for a short stretch, and compare only
    inside that window. Upstream drift does not get in, so WHERE IN THE LEVEL
    THE PHYSICS DIVERGES COMES OUT FOR EVERY SECTION.
    """
    out = {"level": level, "status": "OK", "note": "", "segs": 0, "bad": 0,
           "worst_t": None, "worst": 0.0}
    plan = plan_of(level, a.plans)
    gd = read_ref(level)
    if not plan.exists() or not gd:
        out["status"] = "SKIP"
        out["note"] = "no solution" if not plan.exists() else "no reference"
        return out
    objrects = LEVEL_DATA / f"objrects_lv{level}.txt"
    if has_grouped_colliders(objrects) and not groups_args(plan):
        out["status"], out["note"] = "SKIP", f"no {plan.name}.groups*.txt"
        return out
    cuts = json.loads((REF / "cut.json").read_text()) if (REF / "cut.json").exists() else {}
    cut = cuts.get(str(level)) or max(gd)
    tmp = Path(a.tmp)
    tmp.mkdir(parents=True, exist_ok=True)

    starts = []
    t = a.seg_start
    while t + a.seg_len <= cut:
        if t in gd:
            starts.append(t)
        t += a.seg_step
    worst, worst_t, bad = 0.0, None, 0
    for t0 in starts:
        r = gd[t0]
        start = start_fields(t0, r, plan, gd.get(t0 - 1))
        base = tmp / f"seg_lv{level}_{t0}"
        args = [str(objrects), "--replay", str(plan), "--start", start,
                "--out", str(base)]
        if r.get("pmin") and r.get("pmax"):
            args += ["--startband", f"{r['pmin']},{r['pmax']}"]
        args += band_track_args(level, gd)
        trig, grp = LEVEL_DATA / f"triggers_lv{level}.txt", LEVEL_DATA / f"objgroups_lv{level}.txt"
        if trig.exists() and grp.exists():
            args += ["--triggers", str(trig), "--objgroups", str(grp)]
        obb = LEVEL_DATA / f"obb_lv{level}.txt"
        if obb.exists():
            args += ["--obb", str(obb)]
        args += groups_args(plan)
        subprocess.run([a.leveldp] + args, stdout=subprocess.DEVNULL,
                       stderr=subprocess.DEVNULL)
        d = diff_trace(Path(str(base) + ".trace.csv"), REF / f"lv{level}.csv",
                       t0=t0, t1=min(t0 + a.seg_len, cut), tol=a.tol,
                       limit=10 ** 9)
        if "error" in d:
            continue
        n = len(d["rows"])
        if n:
            bad += 1
            if n > worst:
                worst, worst_t = n, t0
    out["segs"], out["bad"] = len(starts), bad
    out["worst_t"], out["worst"] = worst_t, worst
    return out


def check_level(level: int, a) -> dict:
    """Replay only the model and compare with the reference. Uses no GD, no worker."""
    out = {"level": level, "status": "OK", "note": ""}
    plan = plan_of(level, a.plans)
    ref = REF / f"lv{level}.csv"
    if not plan.exists():
        out["status"], out["note"] = "SKIP", "no solution"
        return out
    if not ref.exists():
        out["status"], out["note"] = "SKIP", "no reference (needs --record)"
        return out
    objrects = LEVEL_DATA / f"objrects_lv{level}.txt"
    if has_grouped_colliders(objrects) and not groups_args(plan):
        out["status"], out["note"] = "SKIP", f"no {plan.name}.groups*.txt"
        return out
    tmp = Path(a.tmp)
    tmp.mkdir(parents=True, exist_ok=True)
    t0 = time.time()
    trace, died, _ = model_replay(level, plan, tmp / f"qr_lv{level}",
                                  Path(a.leveldp), a.with_fixups)
    cut = json.loads((REF / "cut.json").read_text()).get(str(level)) \
        if (REF / "cut.json").exists() else None
    d = diff_trace(trace, ref, t1=cut, tol=a.tol, limit=10 ** 9)
    if "error" in d:
        out["status"], out["note"] = "ERROR", d["error"]
        return out
    rows = d["rows"]
    out["first_t"] = rows[0][0] if rows else None
    out["n_div"] = len(rows)
    out["model_died"] = died
    out["model_ticks"] = d["model_ticks"]
    out["seconds"] = round(time.time() - t0, 2)
    return out


def record_level(level: int, worker_id: int, a) -> dict:
    """Run GD once to build the reference (slow; only called now and then)."""
    plan = plan_of(level, a.plans)
    if not plan.exists():
        return {"level": level, "status": "SKIP", "note": "no solution"}
    tmp = Path(a.tmp)
    tmp.mkdir(parents=True, exist_ok=True)
    dump = tmp / f"qr_ref_lv{level}.dump.csv"
    clear, detail, goal_x = gd_replay(worker_id, level, plan, dump,
                                      a.timeout_minutes * 60, Path(a.workers_root))
    if not dump.exists():
        return {"level": level, "status": "ERROR", "note": detail}
    cut = gd_cut_tick(dump, goal_x)
    n = trim_dump(dump, REF / f"lv{level}.csv")
    return {"level": level, "status": "OK" if clear else "GD-DEAD",
            "note": f"{n} rows, cut={cut}", "cut": cut, "clear": clear}


def verdict(now: list[dict], base: dict, whole: bool,
            slack: int = 0) -> tuple[list[str], bool]:
    """List the items that got worse than the baseline. ONLY THE WORSENING
    DIRECTION IS LOOKED AT (improvement is welcome)."""
    bad = []
    for r in now:
        if r["status"] not in ("OK",):
            continue
        b = base.get(str(r["level"]))
        if not b:
            continue
        lv = r["level"]
        if not whole:
            # compare section by section. leveldp is deterministic, so with the
            # same input and the same anchor BIT-IDENTICAL is the expectation --
            # a drop of even 1 tick is reported (it is not noise; it means the
            # physics changed there)
            ob, nb = b.get("holds") or {}, r.get("holds") or {}
            drops = [(t, ob[t], nb[t]) for t in ob
                     if t in nb and nb[t] < ob[t] - slack]
            if drops:
                drops.sort(key=lambda d: d[2] - d[1])
                worst = drops[0]
                bad.append(f"lv{lv}: tracking shrank in {len(drops)} sections "
                           f"(worst t={worst[0]}: {worst[1]} -> {worst[2]} ticks)")
            continue
        # the model "started dying" / "started dying earlier"
        nd, od = r.get("model_died", -1), b.get("model_died", -1)
        if od < 0 <= nd:
            bad.append(f"lv{lv}: the model now dies (t={nd})")
        elif od >= 0 <= nd and nd < od - 1:
            bad.append(f"lv{lv}: the model dies earlier {od} -> {nd}")
        # the divergence moved earlier
        nf, of_ = r.get("first_t"), b.get("first_t")
        if of_ is None and nf is not None:
            bad.append(f"lv{lv}: a divergence appeared (t={nf})")
        elif of_ is not None and nf is not None and nf < of_ - 1:
            bad.append(f"lv{lv}: the divergence moved earlier t={of_} -> {nf}")
        # the diverging ticks increased (past the larger of 5% and 20 ticks)
        nn, on = r.get("n_div", 0), b.get("n_div", 0)
        if nn > on + max(20, on * 0.05):
            bad.append(f"lv{lv}: more diverging ticks {on} -> {nn}")
    return bad, not bad


def run_segments(a, extra=None):
    """Run the section pass once and return the per-level aggregate.
    (now, number of sections, accumulation of extra)

    If extra(job) is given, it is called ON THE SAME WORKER THREAD right after
    each section's trace is finalised, and the return values (lists) are
    collected and returned. This is the hook that lets fixcensus's census
    evaluation ride along on the same replay (proposal A, 2026-08-18) --
    sections, anchors, exe and the single replay are all completely identical,
    so "the numbers agree with the separated runs" is the acceptance condition
    (see the docstring of py/verify.py).
    """
    now, jobs = [], []
    for lv in a.levels:
        r, js = seg_jobs(lv, a)
        now.append(r)
        jobs += js
    by_lv = {r["level"]: r for r in now}
    holds: dict[int, list] = {lv: [] for lv in by_lv}
    extras: list = []

    def one(j):
        res = run_seg(j, a)
        return res, (extra(j) if extra else None)

    with ThreadPoolExecutor(max_workers=a.parallel) as ex:
        for (lv, seg_t0, hold), e in ex.map(one, jobs):
            holds[lv].append((hold, seg_t0))
            if e:
                extras.extend(e)
    for lv, hs in holds.items():
        if not hs:
            continue
        hs.sort()
        by_lv[lv]["hold_sum"] = sum(h for h, _ in hs)
        by_lv[lv]["hold_min"] = hs[0][0]
        by_lv[lv]["worst_t"] = hs[0][1]
        by_lv[lv]["bad"] = sum(1 for h, _ in hs if h < a.seg_len)
        # keep the per-section values too. Looking only at the sum, one section
        # shrinking while another grows cancels out and becomes invisible
        # (rebuilding an anchor carries a few ticks of noise, so the sum is
        # easily buried under the sum of that noise)
        by_lv[lv]["holds"] = {str(t): h for h, t in hs}
    now.sort(key=lambda r: r["level"])
    return now, len(jobs), extras


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--levels", nargs="+", type=int,
                    default=list(range(1, 23)))
    ap.add_argument("--plans", default=str(DATA / "solution_lv{}_dp.txt"))
    ap.add_argument("--leveldp", default=str(LEVELDP_EXE))
    ap.add_argument("--tol", type=float, default=0.3)
    ap.add_argument("--with-fixups", action="store_true")
    ap.add_argument("--tmp", default=str(DATA / "tmp_quickregress"))
    ap.add_argument("--parallel", type=int, default=6)
    ap.add_argument("--record", action="store_true",
                    help="run GD and re-record the reference (needs a worker)")
    ap.add_argument("--bless", action="store_true",
                    help="save the current numbers as the baseline")
    ap.add_argument("--whole", action="store_true",
                    help="judge on one whole-level replay (weaker than the "
                         "sectioned default)")
    ap.add_argument("--seg-step", type=int, default=400,
                    help="spacing between anchors (ticks). At the default 400 "
                         "the whole suite takes about a minute and a half; 1500 "
                         "is faster and detects markedly less")
    ap.add_argument("--seg-len", type=int, default=400,
                    help="how long a section is compared for (ticks)")
    ap.add_argument("--seg-start", type=int, default=200)
    ap.add_argument("--seg-slack", type=int, default=0,
                    help="tolerance per section, in ticks. 0 = report a section "
                         "that tracked even one tick less")
    ap.add_argument("--pool", nargs="+", type=int,
                    default=[90, 91, 92, 93, 94, 95])
    ap.add_argument("--workers-root", default=str(WORKERS_ROOT))
    ap.add_argument("--timeout-minutes", type=float, default=6.0)
    a = ap.parse_args(argv)

    # REJECT --bless ON A RESTRICTED RUN AT THE DOOR. bless REPLACES
    # `all_base[key]` WHOLESALE with "the rows of the levels that ran this
    # time", so blessing with --levels narrowed erases the baseline of the
    # levels that did not run. A baseline that has gone is treated as "no
    # baseline" on the next run, and a regression passes silently (2026-08-18,
    # user's suggestion). Refuse before running -- refusing after running
    # wastes 2.6 minutes.
    if a.bless and set(a.levels) != set(range(1, 23)):
        print("--bless is only accepted on a full 22-level run "
              "(blessing a --levels subset erases the baseline of every level "
              "that did not run)")
        return 2

    REF.mkdir(parents=True, exist_ok=True)
    t0 = time.time()

    if a.record:
        cuts = {}
        if (REF / "cut.json").exists():
            cuts = json.loads((REF / "cut.json").read_text())
        pool = a.pool
        res = []
        with ThreadPoolExecutor(max_workers=len(pool)) as ex:
            futs = {ex.submit(record_level, lv, pool[i % len(pool)], a): lv
                    for i, lv in enumerate(a.levels)}
            for f in futs:
                r = f.result()
                res.append(r)
                if r.get("cut") is not None:
                    cuts[str(r["level"])] = r["cut"]
        (REF / "cut.json").write_text(json.dumps(cuts, indent=1))
        for r in sorted(res, key=lambda r: r["level"]):
            print(f"lv{r['level']:<3} {r['status']:<9} {r['note']}")
        print(f"--- reference updated in {REF} ({time.time() - t0:.1f}s) ---")
        return 0

    if a.whole:
        with ThreadPoolExecutor(max_workers=a.parallel) as ex:
            now = list(ex.map(lambda lv: check_level(lv, a), a.levels))
        now.sort(key=lambda r: r["level"])
    else:
        now, _, _ = run_segments(a)
    return report(now, a, time.time() - t0)


def report(now: list, a, elapsed: float) -> int:
    """Print the baseline comparison, decide PASS/FAIL, and bless. now is the
    aggregate from run_segments / check_level. verify.py (the unified driver of
    proposal A) calls this same function."""
    key = "baseline_whole" if a.whole else "baseline_seg"
    all_base = json.loads(BASELINE.read_text()) if BASELINE.exists() else {}
    base = all_base.get(key, {})
    if a.whole:
        print(f"{'lv':<5}{'status':<9}{'first div':<12}{'div ticks':<10}"
              f"{'model died':<12}{'vs baseline':<24}note")
    else:
        print(f"{'lv':<5}{'status':<9}{'segs':<7}{'tracked':<11}{'shortest':<10}"
              f"{'at':<12}{'diverged':<11}{'vs baseline':<26}note")
    for r in now:
        if r["status"] != "OK":
            print(f"lv{r['level']:<3} {r['status']:<9}{'':<56}{r['note']}")
            continue
        b = base.get(str(r["level"]), {})
        if a.whole:
            parts = []
            if b:
                if b.get("first_t") != r.get("first_t"):
                    parts.append(f"div {b.get('first_t')}->{r.get('first_t')}")
                if b.get("n_div") != r.get("n_div"):
                    parts.append(f"ticks {b.get('n_div')}->{r.get('n_div')}")
                if b.get("model_died") != r.get("model_died"):
                    parts.append(f"died {b.get('model_died')}->{r.get('model_died')}")
            d = " / ".join(parts) if parts else ("same" if b else "")
            print(f"lv{r['level']:<3} {'OK':<9}{str(r.get('first_t')):<12}"
                  f"{r.get('n_div', 0):<10}{r.get('model_died', -1):<10}{d:<24}"
                  f"{r['note']}")
        else:
            parts = []
            if b:
                if b.get("hold_sum") != r.get("hold_sum"):
                    parts.append(f"tracked {b.get('hold_sum')}->{r.get('hold_sum')}")
                if b.get("bad") != r.get("bad"):
                    parts.append(f"diverged {b.get('bad')}->{r.get('bad')}")
            d = " / ".join(parts) if parts else ("same" if b else "")
            print(f"lv{r['level']:<3} {'OK':<9}{r['segs']:<7}"
                  f"{r.get('hold_sum', 0):<11}{r.get('hold_min', 0):<7}"
                  f"{str(r['worst_t']):<12}{r['bad']:<11}{d:<26}{r['note']}")

    bad, ok = verdict(now, base, a.whole, a.seg_slack)
    print(f"--- {len([r for r in now if r['status'] == 'OK'])} levels "
          f"{elapsed:.1f}s ---")
    # PRINT THE VERDICT FIRST, THEN BLESS. --bless used to return before the
    # verdict, so every acceptance paid the same 2.6 minutes twice: "once to
    # judge it, once more to bless it" (2026-08-18).
    def do_bless() -> None:
        cols = (("first_t", "n_div", "model_died", "model_ticks") if a.whole
                else ("segs", "bad", "hold_sum", "hold_min", "worst_t", "holds"))
        all_base[key] = {str(r["level"]): {k: r.get(k) for k in cols}
                         for r in now if r["status"] == "OK"}
        BASELINE.write_text(json.dumps(all_base, indent=1))
        print(f"baseline updated: {BASELINE} [{key}]")

    if not base:
        print("no baseline yet -- run with --bless to make these the baseline")
        if a.bless:
            do_bless()
        return 0
    if ok:
        print("PASS (nothing worse than the baseline)")
        if a.bless:
            do_bless()
        return 0
    print("FAIL:")
    for b in bad:
        print("  " + b)
    # --bless still writes the baseline unconditionally, as before (a
    # deliberate regression is sometimes accepted). The only difference is that
    # THE CONTENTS OF THE FAIL ARE ALWAYS SEEN BEFORE IT IS WRITTEN.
    if a.bless:
        do_bless()
        return 0
    return 1


if __name__ == "__main__":
    sys.exit(main())
