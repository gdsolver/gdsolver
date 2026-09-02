"""Walk a level's section windows, cross each one with GD, and write the tables.

    python py/secqueue.py --level 22                 # the whole list, Wine
    python py/secqueue.py --level 22 --limit 3       # the top 3 by priority
    python py/secqueue.py --level 22 --venue windows # ...on a Windows worker

The windows come from py/sections.py (fixups, census, deaths, veto boxes). For
each one the section solver is asked to get from the window's entry tick to the
window's exit x, using GD itself as the transition function; the input sequence
it finds is spliced into the plan and replayed on BOTH sides, so the output per
window is "did GD get through, and where does the model disagree with GD along
the path it got through by".

WHAT IS AND IS NOT COLD. This is a measurement pass, not a solve: the verified
solution is replayed to place the entry checkpoint, and it is the spine that
keeps a window from coming back EXHAUSTED for want of the right dedupe
representative. Nothing here produces a solution or feeds one to the solver.

THE SPINE IS A CONFIGURATION GATE, NOT A RESULT. A window whose spine stops
tracking the head run before the exit is reported INVALID-CONFIG and its diff
table is left out of the aggregate: when the pinned rollout drifts, the search
is no longer exploring from the state it believes it is, and its divergences say
more about the harness than about the model. This is the standing invariant of
brief-017 part C written into the data format, so that a night that quietly
loses it cannot be read in the morning as a night that measured something.
"""
from __future__ import annotations

import argparse
import csv
import hashlib
import json
import re
import subprocess
import sys
import time
from pathlib import Path

_PY = Path(__file__).resolve().parent
sys.path.insert(0, str(_PY))
sys.path.insert(0, str(_PY.parent / "mcp"))     # gdmcp.data.diff_trace

import fidelity_diff as FD                                     # noqa: E402
import quick_regress as QR                                     # noqa: E402
from gdmcp.data import diff_trace                              # noqa: E402
from gdtas import plan as P                                    # noqa: E402
from gdtas.paths import DATA, LEVEL_DATA, LEVELDP_EXE, WORKERS_ROOT  # noqa: E402

# The gate: how far the spine may drift from the head run and still count.
SPINE_TOL = 0.05
# Divergence tolerance for the model-vs-GD table (fidelity_diff's own default).
DIFF_TOL = 0.5

# notrace=1 by default: a search writes 32 MB of trace and 15 MB of dump per
# window for a trajectory nobody reads, and the two replays that DO need a dump
# strip the key (see window_diff and the head run).
BASE = ["attempts=1", "quitwhendone=1", "blockinput=1", "cbs=0", "cos=1",
        "fastdt=0.0166667", "fastloops=1", "skiprender=1", "solve=0",
        "music=mute", "coins=0", "notrace=1"]
# The replays do not need fastloops=1. That is the SEARCH's requirement -- a
# checkpoint can only be dropped on a frame boundary, so one tick per call is
# what makes `checkpointat` land on the tick asked for. A plain replay at
# fastloops=1 runs the level in real time (9 minutes for lv22); at 1800, which
# is what fidelity_diff uses, it is under a minute, and over 57 windows with two
# replays each that is the difference between a night and two.
REPLAY_BASE = [c if c != "fastloops=1" else "fastloops=1800"
               for c in BASE if c != "notrace=1"]


def log(m: str) -> None:
    print(f"[{time.strftime('%H:%M:%S')}] {m}", flush=True)


def say_container_mod(WS) -> None:
    """Name the mod the container will launch, without touching it.

    wine_suite.check_deployed() exits when the container's mod is not
    byte-identical to the local build, which is the right default for a run that
    deployed one -- but a run that deliberately did not deploy must still say
    what it is about to measure. A run whose log does not name its binary is not
    a measurement.
    """
    dst = WS.WINE_WORKER / "geode" / "mods" / WS.BUILD_MOD.name
    if not dst.exists():
        log(f"mod in container: {dst} IS MISSING -- pass --deploy")
        return
    b = dst.read_bytes()
    sha = hashlib.sha256(b).hexdigest()[:16]
    same = WS.BUILD_MOD.exists() and b == WS.BUILD_MOD.read_bytes()
    log(f"mod in container: {dst} ({len(b)} B, sha {sha}) "
        + ("== the local build" if same
           else f"!! DIFFERS from {WS.BUILD_MOD} -- measuring the container's, "
                "not the build tree's"))


def say_exe(path: str) -> bool:
    """Name the binary about to be measured, with its mtime. See quick_regress.

    The default is the GEODE-built leveldp, which `cmake --build build-dp` does
    not touch. That has cost this project three separate misreadings, the last
    of them an hour spent on a new dp flag that "printed nothing" because the
    harness was running yesterday's binary.
    """
    p = Path(path)
    if not p.exists():
        log(f"leveldp: {p} DOES NOT EXIST")
        return False
    st = p.stat()
    log(f"leveldp: {p} "
        f"({time.strftime('%Y-%m-%d %H:%M:%S', time.localtime(st.st_mtime))}, "
        f"{st.st_size} B)")
    return True


def read_rows(path: Path) -> dict[int, dict]:
    """tick -> the whole GD row. The anchor builder wants all of it."""
    out: dict[int, dict] = {}
    with path.open(newline="", encoding="utf-8-sig", errors="replace") as f:
        for r in csv.DictReader(f):
            try:
                out[int(r["tick"])] = r
            except (KeyError, ValueError, TypeError):
                continue
    return out


def read_dump(path: Path) -> dict[int, tuple[float, float]]:
    """tick -> (x, y) from a GD dump."""
    return {t: (float(r["x"]), float(r["y"]))
            for t, r in read_rows(path).items()}


def plan_held(inputs: list[tuple[int, int]], t: int) -> int:
    """What the plan is holding at absolute tick t."""
    held = 0
    for step, down in inputs:
        if step > t:
            break
        held = down
    return held


def spine_gate(lines: list[str], head: dict[int, tuple[float, float]],
               want_depth: int) -> dict:
    """Read the `robodbg` lines back and say whether the spine held to the exit.

    The search's own lines are labelled with the tick they correspond to (see
    the note where they are printed), so this is a straight lookup into the head
    run's dump -- no alignment to decide here, which is the point of having the
    mod do the labelling.
    """
    worst, worst_t, deepest, seen = 0.0, -1, -1, 0
    for ln in lines:
        m = re.search(r"robodbg: t=(\d+) sec=1 d=(-?\d+) y=([-\d.]+)", ln)
        if not m:
            continue
        t, d, y = int(m.group(1)), int(m.group(2)), float(m.group(3))
        if t not in head:
            continue
        seen += 1
        deepest = max(deepest, d)
        dy = abs(y - head[t][1])
        if dy > worst:
            worst, worst_t = dy, t
    # want_depth is the reported depth MINUS ONE. The last layer stops at the
    # first leaf that verifies -- the search breaks out of the node loop there --
    # so if the crossing node is stepped before the spine's node in that layer,
    # the spine never takes its own step at the final depth. Measured on the
    # first window: depth 584, spine samples 1..583, worst |dy| 5e-05. Requiring
    # 584 called a perfectly tracking spine BROKEN.
    ok = seen > 0 and worst <= SPINE_TOL and deepest >= want_depth
    return {"ok": ok, "worst_dy": round(worst, 6), "worst_t": worst_t,
            "depth": deepest, "samples": seen, "want_depth": want_depth}


def splice(inputs: list[tuple[int, int]], base: int,
           seq: list[int]) -> list[tuple[int, int]]:
    """chain_secsolve's splice, unchanged: depth i is absolute tick base+i."""
    kept = [(t, d) for t, d in inputs if t < base]
    held = kept[-1][1] if kept else 0
    for i, v in enumerate(seq):
        if v != held:
            kept.append((base + i, v))
            held = v
    return kept


def branch_depth(inputs: list[tuple[int, int]], base: int,
                 seq: list[int]) -> int:
    """First point at which the crossing differs from the verified solution.

    The returned index is 0-based over the sequence, which is depth-1: the input
    at index i is the absolute tick base+i, and the layer at depth d applies the
    plan's input at base+d-1 (measured, see secspineoff). The two agree, which is
    the only reason this comparison means anything.

    -1 means the search came back along the solution's own inputs. Anything else
    is a window where GD got through by a DIFFERENT route, which is where an
    over-kill in the model would show up: the loop's path is not the only one.
    """
    for i, v in enumerate(seq):
        if v != plan_held(inputs, base + i):
            return i
    return -1


def run_window(a, win: dict, inputs: list[tuple[int, int]],
               head: dict[int, tuple[float, float]], session, out_dir: Path) -> dict:
    t0, t1 = win["t0"], win["t1"]
    if t1 not in head or t0 not in head:
        return {"t0": t0, "t1": t1, "verdict": "NO-HEAD"}
    target = head[t1][0]
    horizon = t1 - t0 + 40
    cfg = ["enabled=1", f"level={a.level}"] + BASE + [
        f"practiceat={max(1, t0 - 100)}", f"checkpointat={t0}",
        "secsolve=1", "seclog=1", f"secstart={t0}", f"sectarget={target:.3f}",
        f"sechorizon={horizon}", f"seccap={a.cap}",
        f"robodbg={t0},{t1}"]
    # The search's own clock (secdeadline). A caller's timeout kills the session
    # and the verdict line with it -- last night 5 of 7 windows came back
    # NO-VERDICT, carrying nothing but "not within 90 minutes".
    if a.deadline > 0:
        cfg += [f"secdeadline={a.deadline:.0f}"]
    cfg += [f"input={t},{d}" for t, d in inputs]
    t_start = time.time()
    res = session(cfg, a.timeout)
    out = {"t0": t0, "t1": t1, "targetX": round(target, 2), "cap": a.cap,
           "horizon": horizon, "priority": win.get("priority"),
           "sources": win.get("sources"), "verdict": "NO-VERDICT",
           "wall_s": round(time.time() - t_start, 1)}
    seq: list[int] = []
    for ln in res.lines:
        m = re.search(r"secsolve: (\w+) .*?depth=(\d+)/.*?ms=(\d+) .*?"
                      r"inputBase=(-?\d+).*?foundX=([-\d.]+) foundTick=(-?\d+)", ln)
        if m:
            out.update(verdict=m.group(1), depth=int(m.group(2)),
                       ms=int(m.group(3)), base=int(m.group(4)),
                       foundX=float(m.group(5)), foundTick=int(m.group(6)))
            # WHY it stopped, carried alongside WHAT it concluded. EXHAUSTED
            # says both "no continuation exists" and "the budget ran out"
            # (secdeadline's own clock, `stop=deadline` or `stop=projected`),
            # and a night of budget stops reads as a night of impassable
            # windows unless the record keeps them apart.
            ms = re.search(r"stop=(\w+)", ln)
            out["stop"] = ms.group(1) if ms else "none"
        m = re.search(r"secsolve_inputs: ([01,]+)", ln)
        if m:
            seq = [int(c) for c in m.group(1).split(",") if c]
    out["spine"] = spine_gate(res.lines, head, max(0, out.get("depth", 0) - 1))
    if out["verdict"] == "SOLVED" and seq:
        base = out["base"]
        out["branchDepth"] = branch_depth(inputs, base, seq)
        plan_out = out_dir / f"w{t0}.plan.txt"
        P.write_plan(plan_out, P.format_plan(splice(inputs, base, seq)))
        out["plan"] = plan_out.name
        if not out["spine"]["ok"]:
            out["verdict"] = "INVALID-CONFIG"
        elif not a.no_diff:
            out["diff"] = window_diff(a, plan_out, out_dir / f"w{t0}", t0,
                                      out["foundTick"], session)
    elif out["verdict"] == "SOLVED":
        out["verdict"] = "NO-INPUTS"
    return out


def window_diff(a, plan_out: Path, out_base: Path, t0: int, t1: int,
                session) -> dict:
    """Model vs GD along the path GD actually got through by, within the window.

    THE MODEL IS ANCHORED AT THE WINDOW ENTRY, not run from the head of the
    level. The first version replayed the spliced plan from t=0 on both sides,
    on the reasoning that an anchor is where this kind of measurement usually
    goes wrong. Measured: it produced common=934, rows=0 for BOTH of the night's
    first two windows -- IDENTICAL STATISTICS FOR DIFFERENT WINDOWS, which is
    what made it visible. The model dies at t=934 replaying lv22's own verified
    solution without fixups, thousands of ticks before either window, so "no
    divergences in 5,387..5,973" meant the model was never there. An empty table
    for that reason reads exactly like agreement.

    So: the anchor is built from THIS window's own dump (a crossing that leaves
    the solution's route is a different worldline from gdref, and the columns
    the anchor needs are all in the dump), with the same field builder,
    startband, band tracking and rotation anchor that quick_regress's sections
    use. Reusing that builder rather than writing a second one is deliberate --
    it carries dual, the rotation frame, the stair snap and the robot hover
    budget, each of which was added to it because leaving it out silently
    mismeasured a whole section of a level.
    """
    dump_dst, got_groups = gd_window_replay(a, plan_out, out_base, session)
    if dump_dst is None:
        return {"error": "no dump"}
    if not got_groups:
        return {"error": "no grouptrace - model would see static geometry"}
    return anchored_diff(a, plan_out, out_base, dump_dst, t0, t1)


def gd_window_replay(a, plan_out: Path, out_base: Path,
                     session) -> tuple[Path | None, bool]:
    """Replay the crossing in GD, keeping the dump AND the moving geometry.

    grouptrace=1: the recording is made ON THIS RUN. The level's canonical
    recording is a DIFFERENT WORLDLINE once the crossing leaves the solution's
    route, and a model replay against the wrong world returns nothing but
    divergences -- which reads as a rich table and means nothing.

    The recording is KEPT (24 MB a window). Deleting it after the table cost a
    night: when the table turned out to be measuring nothing, every window had
    to be replayed again to recompute it. Cheap to keep, expensive to refetch.
    """
    dump_dst = Path(str(out_base) + ".dump.csv")
    cfg = (["enabled=1", f"level={a.level}", "grouptrace=1"] + REPLAY_BASE
           + P.read_input_lines(plan_out))
    res = session(cfg, a.timeout)
    if not FD.copy_held_file(res.data_root / "dump.csv", dump_dst):
        return None, False
    groups = Path(str(plan_out) + ".groups.live.txt")
    for name in ("grouptrace_last.txt", "grouptrace.txt"):
        if FD.copy_held_file(res.data_root / name, groups):
            return dump_dst, True
    return dump_dst, False


def anchor_fields(t0: int, r: dict, plan_path: Path, gd: dict[int, dict],
                  inputs: list[tuple[int, int]]) -> str:
    """start_fields, with the anchor's INPUT STATE built the way the model's own
    replay builds it.

    THIS IS brief-019's HARNESS SIDE. The model's --replay does a "pre-anchor
    edge split" (cli.hpp, `preLevel` / `preRise`): the plan's edges are press
    ticks, and an input takes effect latOf(mode) ticks later -- 2 for ship and
    UFO, 1 for everything else -- so the level AT the anchor is computed by
    EFFECT tick, per mode. start_fields instead fills `held` from
    held_before(plan, t0), which counts by press tick, and it carries no pending
    flip at all.

    A search has only the anchor, so both omissions land on it:

      - the level by press tick can say the button was down when its effect has
        not arrived, or vice versa
      - a SWING tapped on the tick whose effect lands at t0 owes a flip on the
        NEXT tick. The dump has no column for that pending bit; the model
        carries it in rHover, which is free in mode 7. Without it, as the
        replay's own note says, THE MODEL NEVER FLIPS AT ALL.

    Measured, lv22 t=7,190 (swing, GD upsideDown 1 -> 0 at 7,191): with the
    anchor as start_fields builds it, the search's two children are identical
    -- the press edge is invisible and no flip branch exists -- while GD and the
    model's own replay both flip. That is the whole of brief-019's family (A).

    Kept local rather than pushed into start_fields: that builder feeds
    quick_regress's whole baseline and reach_check's anchors, and changing it is
    a measured change of its own rather than a side effect of this one.
    """
    f = QR.start_fields(t0, r, plan_path, gd.get(t0 - 1), gd).split(",")
    mode = int(f[4])
    lat = 2 if mode in (1, 3) else 1
    level, rise = 0, False
    for press, v in inputs:
        if press > t0:
            break
        eff = press + lat
        if eff <= t0:
            rise = (eff == t0) and v != 0 and level == 0
            level = v
    f[6] = str(level)
    if mode == 7 and rise:
        f[15] = "1"      # the swing's pending flip rides rHover
    return ",".join(f)


def model_args(a, plan_out: Path) -> list[str]:
    """The level's tables and recordings, the same for every model invocation."""
    objrects = LEVEL_DATA / f"objrects_lv{a.level}.txt"
    args = [str(objrects)]
    trig = LEVEL_DATA / f"triggers_lv{a.level}.txt"
    grp = LEVEL_DATA / f"objgroups_lv{a.level}.txt"
    if trig.exists() and grp.exists():
        args += ["--triggers", str(trig), "--objgroups", str(grp)]
    obb = LEVEL_DATA / f"obb_lv{a.level}.txt"
    if obb.exists():
        args += ["--obb", str(obb)]
    return args + FD.groups_args(plan_out) + QR.ctrlwin_args(a.level)


def reach_probe(a, plan_path: Path, out_base: Path, gd: dict[int, dict],
                t0: int, t1: int, inputs=None) -> dict:
    """Can the MODEL'S REACHABILITY get through this window, from GD's own state?

    The diff table cannot answer this and it is worth being blunt about why: a
    replay measures fidelity ALONG A PATH THAT WAS TAKEN. It can show the model
    disagreeing about where the player ends up; it can never show a state the
    model forbids, because a forbidden state simply is not in the replay.

    Here the DP is run forward from the window entry with the window's own
    length as its horizon. The verified solution demonstrably gets through every
    one of these windows in GD, so a frontier that dies inside the window is the
    model refusing a passage that exists -- an over-kill candidate, located by
    the tick and x where the frontier died.

    Offline: no GD, no worker, seconds per window.
    """
    r = gd.get(t0)
    if not r:
        return {"error": f"no reference row at t={t0}"}
    out = Path(str(out_base) + ".reach")
    args = ([str(a.leveldp)] + model_args(a, plan_path)
            + ["--out", str(out), "--cap", str(a.reachcap),
               "--shipyq", "0.25", "--shipvq", "1.0",
               "--threads", str(a.threads),
               "--start", (anchor_fields(t0, r, plan_path, gd, inputs)
                           if inputs is not None
                           else QR.start_fields(t0, r, plan_path,
                                                gd.get(t0 - 1), gd)),
               "--horizon", str(t1 - t0)])
    if r.get("pmin") and r.get("pmax"):
        args += ["--startband", f"{r['pmin']},{r['pmax']}"]
    args += QR.band_track_args(a.level, gd)
    args += QR.rot_anchor_args(a.level, t0)
    t = time.time()
    p = subprocess.run(args, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL,
                       text=True, encoding="utf-8", errors="replace")
    verdict, deep_t, deep_x = "FAILED", -1, -1.0
    for ln in (p.stdout or "").splitlines():
        m = re.match(r"^(PARTIAL|FAILED): frontier died at t=(\d+) x=([\d.\-]+)",
                     ln)
        if m:
            verdict, deep_t, deep_x = m.group(1), int(m.group(2)), float(m.group(3))
            continue
        if ln.startswith("SOLVED at") and verdict != "PARTIAL":
            verdict = "SOLVED"
    rec = {"verdict": verdict, "diedT": deep_t, "diedX": deep_x,
           "t0": t0, "t1": t1, "held": deep_t - t0 if deep_t > 0 else None,
           "cap": a.reachcap, "seconds": round(time.time() - t, 1)}
    # A frontier that died PAST the window's exit crossed the window. Measured:
    # 4 of the first sweep's 10 PARTIALs are this -- t0=1,966 died at t=3,056
    # with an exit at 2,593. Reporting those as "the model cannot cross" would
    # have put four windows on the over-kill list that the model gets through.
    if verdict == "PARTIAL" and deep_t >= t1:
        rec["verdict"] = "CROSSED"
    # reach_check's own caveat, kept here rather than left for a reader to
    # remember: in a rotation section the anchor cannot carry 2900's one-shot
    # state or the phase of autonomous triggers, so a frontier that dies within
    # a few ticks of the anchor is telling us about the anchor, not the model.
    if verdict != "SOLVED" and rec["held"] is not None and rec["held"] <= 5:
        rec["verdict"] = "ANCHOR-WEAK"
    return rec


def anchored_diff(a, plan_out: Path, out_base: Path, dump: Path,
                  t0: int, t1: int) -> dict:
    """The model side, started at t0 from GD's own row, and the table.

    Split out from the GD replay so it can be recomputed from files that are
    already on disk (`--rediff`), without asking GD for the window again.
    """
    gd = read_rows(dump)
    r = gd.get(t0)
    if not r:
        return {"error": f"dump has no row at t={t0}"}
    objrects = LEVEL_DATA / f"objrects_lv{a.level}.txt"
    args = [str(objrects), "--replay", str(plan_out)]
    trig = LEVEL_DATA / f"triggers_lv{a.level}.txt"
    grp = LEVEL_DATA / f"objgroups_lv{a.level}.txt"
    if trig.exists() and grp.exists():
        args += ["--triggers", str(trig), "--objgroups", str(grp)]
    obb = LEVEL_DATA / f"obb_lv{a.level}.txt"
    if obb.exists():
        args += ["--obb", str(obb)]
    args += FD.groups_args(plan_out)
    args += QR.ctrlwin_args(a.level)
    args += ["--start", QR.start_fields(t0, r, plan_out, gd.get(t0 - 1), gd),
             "--out", str(out_base)]
    if r.get("pmin") and r.get("pmax"):
        args += ["--startband", f"{r['pmin']},{r['pmax']}"]
    args += QR.band_track_args(a.level, gd)
    args += QR.rot_anchor_args(a.level, t0)
    p = subprocess.run([a.leveldp] + args, stdout=subprocess.PIPE, text=True,
                       errors="replace")
    m = re.search(r"REPLAY: model DIED at t=(\d+)", p.stdout)
    died = int(m.group(1)) if m else -1
    trace = Path(str(out_base) + ".trace.csv")
    if not trace.exists():
        return {"error": "no model trace", "modelDied": died}
    d = diff_trace(trace, dump, t0=t0, t1=t1, tol=DIFF_TOL, limit=10 ** 9)
    rows = d.get("rows") or []
    out = {"anchoredAt": t0, "modelDied": died, "common": d.get("common"),
           "verdictNote": d.get("verdict"),
           "firstDiv": d.get("first_divergence"), "cols": d.get("cols"),
           "rows": len(rows), "table": rows[:40]}
    # An anchored replay that covers none of the window is not agreement. Say so
    # in the record rather than leaving a zero to be read as one.
    if not d.get("common"):
        out["error"] = "model covered no tick of the window"
    return out


def reach_sweep(a, wins: list[dict], out_dir: Path, plan_path: Path,
                inputs) -> int:
    """Every window, offline: does the model's reachability cross it?

    Anchored on the HEAD run -- the verified solution's own worldline -- because
    that is the trajectory known to get through every window in GD. A window the
    model cannot cross from there is the model forbidding a passage that exists.
    """
    head = out_dir / "head.dump.csv"
    if not head.exists():
        log(f"no {head}: run the queue once (or --venue to make one)")
        return 2
    gd = read_rows(head)
    index = out_dir / "reach.jsonl"
    if index.exists():
        index.unlink()
    tally: dict[str, int] = {}
    for i, w in enumerate(wins, 1):
        rec = reach_probe(a, plan_path, out_dir / f"w{w['t0']}", gd,
                          w["t0"], w["t1"], inputs)
        rec["priority"] = w.get("priority")
        rec["sources"] = w.get("sources")
        tally[rec.get("verdict", "?")] = tally.get(rec.get("verdict", "?"), 0) + 1
        with index.open("a", encoding="utf-8") as f:
            f.write(json.dumps(rec, separators=(",", ":")) + "\n")
        log(f"[{i}/{len(wins)}] t0={w['t0']}..{w['t1']} {rec.get('verdict')}"
            f" held={rec.get('held')} diedX={rec.get('diedX')}"
            f" {rec.get('seconds')}s")
    log(f"reach: {tally} -> {index}")
    return 0


def refwatch_sweep(a, wins: list[dict], out_dir: Path, plan_path: Path,
                   inputs) -> int:
    """Every window: which gate drops the reference, if any (brief-019's (3)).

    Two runs per window, both offline: an anchored replay of the solution to
    make the reference, then the search with --refwatch. What matters most is
    not the gates but the COUNT OF `cannot-reproduce` -- that is the search and
    the replay disagreeing about physics rather than about pruning, and the
    acceptance for brief-019 is that it reaches zero across the whole level.
    """
    head = out_dir / "head.dump.csv"
    if not head.exists():
        log(f"no {head}")
        return 2
    gd = read_rows(head)
    index = out_dir / "refwatch.jsonl"
    if index.exists():
        index.unlink()
    tally: dict[str, int] = {}
    for i, w in enumerate(wins, 1):
        t0, t1 = w["t0"], w["t1"]
        r = gd.get(t0)
        if not r:
            continue
        anchor = anchor_fields(t0, r, plan_path, gd, inputs)
        tail = []
        if r.get("pmin") and r.get("pmax"):
            tail += ["--startband", f"{r['pmin']},{r['pmax']}"]
        tail += QR.band_track_args(a.level, gd) + QR.rot_anchor_args(a.level, t0)
        ref = out_dir / f"ref{t0}"
        subprocess.run([a.leveldp] + model_args(a, plan_path)
                       + ["--replay", str(plan_path), "--out", str(ref),
                          "--start", anchor] + tail,
                       stdout=subprocess.DEVNULL, text=True)
        p = subprocess.run([a.leveldp] + model_args(a, plan_path)
                           + ["--out", str(out_dir / f"rw{t0}"),
                              "--cap", str(a.reachcap), "--shipyq", "0.25",
                              "--shipvq", "1.0", "--threads", str(a.threads),
                              "--start", anchor, "--horizon", str(t1 - t0),
                              "--refwatch", str(ref) + ".trace.csv"] + tail,
                           stdout=subprocess.PIPE, text=True, errors="replace")
        gate, line = "CARRIED", ""
        for ln in (p.stdout or "").splitlines():
            m = re.search(r"refwatch: LOST t=(\d+) gate=([a-z-]+)", ln)
            if m:
                gate, line = m.group(2), ln.strip()
                break
        rec = {"t0": t0, "t1": t1, "gate": gate, "line": line[:300],
               "priority": w.get("priority")}
        tally[gate] = tally.get(gate, 0) + 1
        with index.open("a", encoding="utf-8") as f:
            f.write(json.dumps(rec, separators=(",", ":")) + "\n")
        log(f"[{i}/{len(wins)}] t0={t0} {gate}")
    log(f"refwatch: {tally} -> {index}")
    bad = tally.get("cannot-reproduce", 0)
    log(f"brief-019 acceptance (3): cannot-reproduce = {bad}"
        + ("  PASS" if bad == 0 else "  not yet"))
    return 0


def rediff(a, wins: list[dict], out_dir: Path, session=None) -> int:
    """Recompute the tables for windows already on disk. GD is not asked again.

    A crossing costs 15 to 90 minutes of GD; the model side of the table costs
    under a second. Keeping the two separable is what made the night's first
    defect recoverable instead of a night thrown away -- the plan, the dump and
    the recording of every crossed window are all still there.
    """
    done = 0
    for w in wins:
        t0 = w["t0"]
        jf = out_dir / f"w{t0}.json"
        if not jf.exists():
            continue
        rec = json.loads(jf.read_text("utf-8"))
        if rec.get("verdict") != "SOLVED" or not rec.get("foundTick"):
            continue
        plan_out = out_dir / f"w{t0}.plan.txt"
        dump = out_dir / f"w{t0}.dump.csv"
        if not plan_out.exists() or not dump.exists():
            log(f"w{t0}: plan or dump missing, skipping")
            continue
        if not FD.groups_args(plan_out):
            # The recording is what an early version of this deleted. Ask GD for
            # the replay again -- a minute -- rather than skip the window or,
            # worse, run the model against static geometry.
            if session is None:
                log(f"w{t0}: no recording and no venue to remake it, skipping")
                continue
            log(f"w{t0}: recording missing, replaying to remake it")
            dump, ok = gd_window_replay(a, plan_out, out_dir / f"w{t0}", session)
            if not ok:
                log(f"w{t0}: replay produced no recording, skipping")
                continue
        rec["diff"] = anchored_diff(a, plan_out, out_dir / f"w{t0}", dump,
                                    t0, rec["foundTick"])
        jf.write_text(json.dumps(rec, indent=1), encoding="utf-8")
        d = rec["diff"]
        log(f"w{t0}: anchored at {t0} died={d.get('modelDied')} "
            f"common={d.get('common')} firstDiv={d.get('firstDiv')} "
            f"rows={d.get('rows')}{' ' + d['error'] if d.get('error') else ''}")
        done += 1
    log(f"rediff: {done} windows")
    return 0


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--level", type=int, required=True)
    ap.add_argument("--deploy", action="store_true",
                    help="replace the container's mod with the local build "
                         "before running. OFF by default: a night queue may be "
                         "running under it (2026-09-02 cost 13 windows)")
    ap.add_argument("--venue", choices=("wine", "windows"), default="wine",
                    help="Wine is the default: a night-long queue is what that "
                         "standing ruling exists for")
    ap.add_argument("--worker", type=int, default=99)
    ap.add_argument("--cap", type=int, default=100)
    ap.add_argument("--limit", type=int, default=0, help="0 = every window")
    ap.add_argument("--windows", default="", help="comma-separated t0 to run")
    ap.add_argument("--timeout", type=float, default=5400.0)
    ap.add_argument("--deadline", type=float, default=0.0,
                    help="seconds the SEARCH gives itself; it stops at a layer "
                         "boundary and reports (0 = no deadline)")
    ap.add_argument("--leveldp", default=str(LEVELDP_EXE))
    ap.add_argument("--out-dir", default="")
    ap.add_argument("--no-diff", action="store_true",
                    help="crossings only, skip the model/GD tables")
    ap.add_argument("--rediff", action="store_true",
                    help="recompute the tables for windows already crossed, "
                         "from the plan and dump on disk. No GD, no search")
    ap.add_argument("--reach", action="store_true",
                    help="ask whether the MODEL can get through each window "
                         "from GD's own entry state. Offline, every window, "
                         "no crossing needed")
    ap.add_argument("--refsweep", action="store_true",
                    help="every window: which gate drops the reference "
                         "(brief-019 acceptance 3). Offline")
    ap.add_argument("--reachcap", type=int, default=2000)
    ap.add_argument("--threads", type=int, default=8)
    a = ap.parse_args(argv)

    out_dir = Path(a.out_dir) if a.out_dir else LEVEL_DATA / f"secqueue_lv{a.level}"
    out_dir.mkdir(parents=True, exist_ok=True)
    wins = json.loads((LEVEL_DATA / f"sections_lv{a.level}.json").read_text("utf-8"))
    wins.sort(key=lambda w: -w.get("priority", 0))
    if a.windows:
        want = {int(x) for x in a.windows.split(",") if x.strip()}
        wins = [w for w in wins if w["t0"] in want]
    if a.limit:
        wins = wins[:a.limit]

    plan_path = DATA / f"solution_lv{a.level}_dp.txt"
    inputs = [(t, d) for t, d in P.read_inputs(plan_path)]

    if not say_exe(a.leveldp):
        return 2

    # Offline modes: no venue, no GD, no worker.
    if a.refsweep:
        return refwatch_sweep(a, wins, out_dir, plan_path, inputs)
    if a.reach:
        return reach_sweep(a, wins, out_dir, plan_path, inputs)

    if a.venue == "wine":
        sys.path.insert(0, str(Path(r"C:\GD-lab\oneoff\py")))
        import wine_suite as WS
        if a.deploy:
            WS.deploy()
            WS.check_deployed()
        else:
            # DO NOT REWRITE THE CONTAINER'S MOD BY DEFAULT. This used to deploy
            # on every start, which is silent, sounds harmless and is not: on
            # 2026-09-02 a `--rediff` run -- an offline re-diff, no game intended
            # -- replaced the mod under a night queue that had been running for
            # four hours, and 13 of its 38 completed windows were measured with a
            # different solver than the first 25 (the mod embeds dp). secnight
            # already deploys once itself and launches its windows through
            # secqueue_nodeploy.py for exactly this reason; this makes the plain
            # entry point agree with it.
            # The check still SAYS what is being measured, loudly, and a mismatch
            # is a line rather than an exit: a session explaining a measurement
            # has to run the binary that made it, and the build tree moves.
            say_container_mod(WS)

        def session(cfg, timeout):
            # Wipe before EVERY session, not once per queue. The dump is copied
            # out of the data dir after the run, and a session that fails to
            # write one would otherwise hand back the previous window's -- the
            # exact shape of "measure the instrument before believing it".
            WS.wine_wipe(a.worker)
            return WS.wine_run_session(a.worker, cfg, timeout_s=timeout,
                                       done_marker="session_end")
    else:
        from gdtas.worker import run_session

        def session(cfg, timeout):
            return run_session(a.worker, cfg, timeout_s=timeout,
                               workers_root=WORKERS_ROOT)

    if a.rediff:
        return rediff(a, wins, out_dir, session)

    # The head run: one plain replay, kept for the whole queue. Every window's
    # target x and every spine comparison is read out of it, so it is measured
    # once on the venue the queue runs on rather than assumed from elsewhere.
    head_dump = out_dir / "head.dump.csv"
    if not head_dump.exists():
        log("head run")
        res = session(["enabled=1", f"level={a.level}"] + REPLAY_BASE
                      + P.read_input_lines(plan_path), a.timeout)
        if not FD.copy_held_file(res.data_root / "dump.csv", head_dump):
            log("FATAL: no head dump")
            return 2
    head = read_dump(head_dump)
    log(f"head: {len(head)} ticks, {len(wins)} windows, venue={a.venue}")

    index = out_dir / "index.jsonl"
    done = set()
    if index.exists():
        for ln in index.read_text("utf-8").splitlines():
            try:
                done.add(json.loads(ln)["t0"])
            except (ValueError, KeyError):
                continue
    for i, w in enumerate(wins, 1):
        if w["t0"] in done:
            log(f"[{i}/{len(wins)}] t0={w['t0']} already done, skipping")
            continue
        log(f"[{i}/{len(wins)}] t0={w['t0']}..{w['t1']} "
            f"(priority {w.get('priority')})")
        # One window must not be able to end the night. A crash here is recorded
        # as this window's verdict and the queue moves on -- the alternative is
        # waking up to a queue that stopped at 01:00 for a reason no line names.
        try:
            rec = run_window(a, w, inputs, head, session, out_dir)
        except Exception as e:                                  # noqa: BLE001
            rec = {"t0": w["t0"], "t1": w["t1"], "verdict": "ERROR",
                   "error": f"{type(e).__name__}: {e}"}
            log(f"    ERROR {rec['error']}")
        (out_dir / f"w{w['t0']}.json").write_text(
            json.dumps(rec, indent=1), encoding="utf-8")
        with index.open("a", encoding="utf-8") as f:
            f.write(json.dumps(rec, separators=(",", ":")) + "\n")
        sp = rec.get("spine", {})
        log(f"    {rec['verdict']}"
            f"{'' if rec.get('stop', 'none') == 'none' else '/' + rec['stop']}"
            f" depth={rec.get('depth')} "
            f"{rec.get('wall_s')}s spine={'ok' if sp.get('ok') else 'BROKEN'}"
            f" worst={sp.get('worst_dy')} branch={rec.get('branchDepth')}"
            f" div={(rec.get('diff') or {}).get('rows')}")
    log(f"queue done -> {index}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
