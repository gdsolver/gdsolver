"""Hand the input to a person at a point of a plan, and measure the route they play.

    python py/takeover.py play --level 16 --plan data/solution_lv16_dp.txt --at 5200 --worker-id 101
    python py/takeover.py play --level 9001 --levelfile <level.lvl> --plan <plan> --at-x 4800 ...
    python py/takeover.py play --level 16 --plan data/solution_lv16_dp.txt --worker-id 101
    python py/takeover.py check <run dir> --worker-id 101

With neither --at nor --at-x the plan plays as an ordinary replay (up/down arrows: speed) and the
takeover key (F8) hands the input over wherever it stands: the checkpoint is put there and the
rest is as below. The key then ends the mode, as it does after a fixed point (mod cfg takeoverkey=1,
off by default).

A debugging instrument (the mod side is src/mod/takeover.hpp). Where the repair loop or a section
search keeps failing, a person can often see a way through that the search does not take. `play`
opens a visible worker that flies the plan to the point, puts the strict checkpoint there (the one
the loop's checkpoint flights use) and gives the jump button to the person. From there the game's
own checkpoints (practice mode: its auto-checkpoint setting, Z, X) are the person's, each replaced
by a strict one; X never removes the start point's. Every death goes back to the newest; a clear,
or the end key (Geode setting key-takeover-end, F8 by default), ends the mode and closes the
worker. Every attempt is written out as a whole plan from tick 0.

`check` then measures each attempt, with no person involved:
  replay   the attempt's plan flown again from the head in a headless worker, against the
           takeover session's own dump. Identical physics columns mean the recording carries the
           route (the presses and their ticks) and the checkpoint restores held; a difference
           names the first tick and column where they part.
  model    the same plan replayed by leveldp against the headless replay's dump (fidelity_diff's
           comparison, gdmcp.data.diff_trace): the first tick where the model leaves GD, and
           whether that is before the point (the plan's part) or after it (the person's).

RULES THIS TOOL KEEPS:
  * It is never a seed. Nothing is written where a solve reads -- no solution file, no resume,
    no fixups. The run directory lives outside the repository (--out). A person's route is not
    evidence that the solver can find one, and is never reported as a solve.
  * The worker stays a bot session the whole time: achievements, statistics, coins and the
    level's record stay blocked, and result.txt must end with `level record changed: none`
    (check prints that line).
  * One worker, one session at a time; the check's replays run on the same worker after the
    person's session has ended.
"""

from __future__ import annotations

import argparse
import csv
import hashlib
import json
import re
import shutil
import subprocess
import sys
import time
from pathlib import Path

_PY = Path(__file__).resolve().parent
sys.path.insert(0, str(_PY))
sys.path.insert(0, str(_PY.parent / "mcp"))

from gdtas import observe                                     # noqa: E402
from gdtas import plan as P                                   # noqa: E402
from gdtas import results as R                                # noqa: E402
from gdtas.paths import (BUILD_MOD, DATA, LEVEL_DATA,         # noqa: E402
                         LEVELDP_EXE, WORKERS_ROOT)
from gdtas.solveutil import copy_held_file, has_grouped_colliders  # noqa: E402
from gdtas.worker import _WorkerBase, run_session             # noqa: E402

# Untracked (data/ is an allow-list), and a folder of its own: nothing that reads solutions looks
# in it. A run with many attempts writes a replay dump per attempt, so --out on a roomier disk is
# worth passing.
DEFAULT_OUT = DATA / "takeover"

# The dump columns a replay must reproduce for the recording to count as the same run: both
# players' physics and what they stand on. Everything else is counted separately and does not
# decide: the camera and the ground layers carry run-to-run noise of their own (measured on the
# checkpoint probes of 2026-09-30), the flight band moves with the camera, and `frame` / `gframe`
# count update calls, which a person's realtime frames and a headless replay batch differently.
PHYS_COLS = ("x", "y", "yvel", "rot", "mode", "upsideDown", "onGround", "onGround2", "dead",
             "speed", "gravityMod", "platXVel", "vsize", "snapuid", "snapdist",
             "dual", "p2x", "p2y", "p2vy", "p2rot", "p2up", "p2ground", "p2ground2", "p2dead",
             "p2mode", "p2vsize")

# The level dumps the model needs (solver::buildPois writes them; cfg clearance=1 asks for it).
LEVEL_DUMPS = ("objrects.txt", "triggers.txt", "objgroups.txt", "obb.txt")
# ...and the ones a custom level's solve hands the model as well, kept when the replay wrote them
# (each is optional: a level without areas or forced blocks writes none).
LEVEL_DUMPS_OPTIONAL = (("levelsettings.txt", "--levelsettings"), ("rotgameplay.txt", "--rotgameplay"),
                        ("forceblocks.txt", "--forceids"), ("areas.txt", "--areas"),
                        ("items.txt", "--items"))


def sha256(p: Path) -> str:
    h = hashlib.sha256()
    with p.open("rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def git(*args: str) -> str:
    r = subprocess.run(["git", "-C", str(_PY.parent), *args], capture_output=True, text=True)
    return r.stdout.strip()


# ---- play -------------------------------------------------------------------------------

def session_cfg(a, data_root: Path) -> list[str]:
    cfg = [
        "enabled=1", f"level={a.level}",
        "solve=0",            # a replay up to the point, then the person
        "blockinput=1",       # the plan drives until the point; the mod lifts it there
        "attempts=999999",    # deaths come back to the checkpoint, not to an end of session
        "quitwhendone=1",     # the end key (or a clear) closes the worker, and play collects
        "skiprender=0", "fastloops=1", "fastdt=0",   # realtime; the approach is the mod's
        "fps=60",             # the worker's save has no vsync: without this it runs ~19x
        "deathfx=0",          # the death effect crashes a spectated run
        "notrace=0",          # dump.csv is the session's own record of every tick
        "cbs=0", "cos=1",     # presses land on steps, so a tick carries one
        f"slowmo={a.slowmo}",
        f"takeoverwait={a.wait}",
    ]
    if a.at_x is not None:
        cfg.append(f"takeoverx={a.at_x}")
    elif a.at is not None:
        cfg.append(f"takeover={a.at}")
    else:
        cfg.append("takeoverkey=1")   # no fixed point: the end key (F8) hands over where it stands
    if a.levelfile:
        cfg.append(f"levelfile={data_root / Path(a.levelfile).name}")
        if a.levelfile_main:
            cfg.append("levelfiletype=main")   # a level file with a main level's physics (the spin-offs)
    return cfg + P.read_input_lines(a.plan)


def newest_takeover_dir(data_root: Path) -> Path | None:
    root = data_root / "takeover"
    if not root.is_dir():
        return None
    dirs = sorted((d for d in root.iterdir() if d.is_dir()), key=lambda d: d.name)
    return dirs[-1] if dirs else None


def cmd_play(a) -> int:
    plan = Path(a.plan)
    if not plan.exists():
        print(f"plan not found: {plan}")
        return 1
    if not P.read_input_lines(plan) and (a.at or 0) > 0:
        print(f"note: {plan.name} has no input= lines -- the plan before the point is empty")
    mod = Path(a.mod)
    stamp = time.strftime("%Y%m%d-%H%M%S")
    run = Path(a.out) / f"lv{a.level}_{stamp}"
    run.mkdir(parents=True, exist_ok=True)
    data_root = _WorkerBase(a.worker_id, Path(a.workers_root)).data
    cfg = session_cfg(a, data_root)
    extra = [Path(a.levelfile)] if a.levelfile else []
    if a.at_x is None and a.at is None:
        print(f"lv{a.level}: replaying {plan.name} as an ordinary replay (up/down arrows: speed).\n"
              f"  press the takeover key (default F8) where you want to take over: a checkpoint\n"
              f"  is put there and the input is yours. Dying returns to it; F8 again or a clear ends it.\n"
              f"  run directory: {run}")
    else:
        point = f"x={a.at_x}" if a.at_x is not None else f"t={a.at}"
        print(f"lv{a.level}: flying {plan.name} to {point}, then the input is yours.\n"
              f"  dying returns to that point; the end key (default F8) or a clear ends it.\n"
              f"  run directory: {run}")
    proc, data = observe.launch_visible(a.worker_id, cfg, a.resolution_index, mod,
                                        kill_others=False, workers_root=Path(a.workers_root),
                                        extra_files=extra)
    seen = 0
    while proc.poll() is None:
        time.sleep(1.0)
        lines = R.read_lines(data / "result.txt")
        for l in lines[seen:]:
            if l.startswith("takeover:"):
                print("  " + l)
        seen = len(lines)
    lines = R.read_lines(data / "result.txt")
    for l in lines[seen:]:
        if l.startswith("takeover:"):
            print("  " + l)

    tk = newest_takeover_dir(data)
    if tk is None:
        print("the session wrote no takeover folder (the point was never reached?)")
    else:
        shutil.copytree(tk, run / "takeover", dirs_exist_ok=True)
    for name in ("dump.csv", "result.txt", "autorun.cfg"):
        if not copy_held_file(data / name, run / name):
            print(f"could not copy {name}")
    shutil.copyfile(plan, run / "plan_in.txt")
    if a.levelfile:
        shutil.copyfile(a.levelfile, run / Path(a.levelfile).name)
    meta = {
        "level": a.level, "levelfile": Path(a.levelfile).name if a.levelfile else "",
        "levelfile_main": bool(a.levelfile and a.levelfile_main),
        "plan": str(plan), "plan_sha256": sha256(plan),
        "at": a.at if a.at_x is None else None, "at_x": a.at_x, "wait": a.wait,
        "slowmo": a.slowmo, "worker": a.worker_id,
        "mod": str(mod), "mod_sha256": sha256(mod) if mod.exists() else "",
        "mod_mtime": time.strftime("%Y-%m-%d %H:%M:%S", time.localtime(mod.stat().st_mtime))
                     if mod.exists() else "",
        "head": git("rev-parse", "HEAD"), "dirty": bool(git("status", "--porcelain")),
        "started": stamp,
    }
    (run / "run.json").write_text(json.dumps(meta, indent=2), encoding="utf-8")
    rec = [l for l in lines if l.startswith("level record changed:")]
    print(rec[-1] if rec else "WARNING: no `level record changed:` line in result.txt")
    if tk is None or a.no_check:
        return 0
    return cmd_check(argparse.Namespace(run=str(run), worker_id=a.worker_id,
                                        workers_root=a.workers_root, mod=a.mod,
                                        leveldp=a.leveldp, timeout_minutes=a.timeout_minutes,
                                        tol=a.tol, no_model=False))


# ---- check ------------------------------------------------------------------------------

def parse_summary(path: Path) -> tuple[dict, dict[int, dict], list[dict]]:
    """summary.txt: the start line, every checkpoint by n (removed ones included -- an attempt
    that started from one before its removal still needs it), and the attempt lines."""
    start: dict = {}
    cps: dict[int, dict] = {}
    attempts: list[dict] = []
    for l in path.read_text(encoding="utf-8", errors="replace").splitlines():
        kv = dict(re.findall(r"(\w[\w-]*)=(\S+)", l))
        if l.startswith("start "):
            start = kv
        elif l.startswith("checkpoint "):
            cps[int(kv["n"])] = {"t": int(kv["t"]), "dump": kv["dump"], "parent": int(kv["parent"])}
        elif l.startswith("attempt="):
            attempts.append(kv)
    return start, cps, attempts


def read_dump(path: Path) -> dict[str, dict[int, dict]]:
    """dump.csv as attempt -> tick -> row (the first row of a tick wins, as best_attempt_rows)."""
    per: dict[str, dict[int, dict]] = {}
    if not path.exists():
        return per
    with path.open(newline="", encoding="utf-8-sig", errors="replace") as f:
        for rec in csv.DictReader(f):
            try:
                t = int(rec["tick"])
            except (KeyError, ValueError, TypeError):
                continue
            per.setdefault(rec.get("attempt", "0"), {}).setdefault(t, rec)
    return per


def session_rows(dump: dict[str, dict[int, dict]], cps: dict[int, dict],
                 att: dict) -> dict[int, dict]:
    """One attempt's run out of the session dump: its own rows after the checkpoint it started
    from (a restored attempt starts writing at T+1), and before that the rows of the attempt that
    took that checkpoint, back through each checkpoint's parent to the plan's own flight."""
    n = int(att.get("cp", 0))
    lo = cps[n]["t"] if n in cps else -1
    rows = {t: r for t, r in dump.get(att.get("dump", ""), {}).items() if t > lo}
    while n in cps:
        c = cps[n]
        p = c["parent"]
        plo = cps[p]["t"] if p in cps else -1
        rows.update({t: r for t, r in dump.get(c["dump"], {}).items() if plo < t <= c["t"]})
        n = p
    return rows


def end_tick(lines: list[str]) -> int | None:
    """The tick of the run's last death or completion line in a result.txt."""
    for l in reversed(lines):
        if l.startswith(R.DEATH) or l.startswith(R.COMPLETE):
            m = re.search(r"\btick=(\d+)", l)
            if m:
                return int(m.group(1))
    return None


def compare_runs(session: dict[int, dict], replay: dict[int, dict],
                 s_end: int | None, r_end: int | None) -> dict:
    """The two runs tick by tick up to the earlier of their ends. Rows after a death or a clear
    are not compared: how many a dead or finished player writes depends on wall time (GD's reset
    is scheduled on the clock), which a realtime session and a headless replay spend differently."""
    cut = min(e for e in (s_end, r_end, max(session, default=0), max(replay, default=0))
              if e is not None)
    common = sorted(t for t in set(session) & set(replay) if t <= cut)
    first = None
    other: dict[str, int] = {}
    for t in common:
        s, r = session[t], replay[t]
        for c in PHYS_COLS:
            if c in s and c in r and s[c] != r[c]:
                if first is None:
                    first = {"tick": t, "column": c, "session": s[c], "replay": r[c]}
                break
        for c in s:
            if c in PHYS_COLS or c in ("frame", "attempt", "tick"):
                continue
            if c in r and s[c] != r[c]:
                other[c] = other.get(c, 0) + 1
    return {
        "ticks_compared": len(common),
        "session_end": s_end,
        "replay_end": r_end,
        "first_physics_difference": first,
        "other_columns_differing": other,
    }


def replay_cfg(meta: dict, data_root: Path, plan: Path, want_level_dumps: bool) -> list[str]:
    cfg = [
        "enabled=1", f"level={meta['level']}", "attempts=1", "quitwhendone=1",
        "blockinput=1", "cbs=0", "cos=1",
        "fastdt=0.0166667", "fastloops=1800", "skiprender=1",
        "solve=0", "music=mute", "coins=0",
        "grouptrace=1",       # the moving geometry along this route, for the model
    ]
    if want_level_dumps:
        cfg.append("clearance=1")   # only as the trigger for buildPois's level dumps
    if meta.get("levelfile"):
        cfg.append(f"levelfile={data_root / meta['levelfile']}")
        if meta.get("levelfile_main"):
            cfg.append("levelfiletype=main")
    return cfg + P.read_input_lines(plan)


def custom_start_args(level_dir: Path, plan: Path, out_base: Path,
                      replay_rows: dict[int, dict]) -> list[str]:
    """What a custom level's head solve hands the model beyond the object dumps, from the check's own
    headless replay: the level files it wrote, the state at its first row (--start, as quick_regress
    builds an anchor) with the band GD had there (--startband), and GD's x per tick (--xtrack, which
    the loop's calls carry too). Without them the model began a level that starts at y 3,525 on the
    ground at 105 and parted from GD on its first row (10-12)."""
    from quick_regress import start_fields
    args: list[str] = []
    for f, flag in LEVEL_DUMPS_OPTIONAL:
        if (level_dir / f).exists():
            args += [flag, str(level_dir / f)]
    if replay_rows:
        t0 = min(replay_rows)
        r0 = replay_rows[t0]
        args += ["--start", start_fields(t0, r0, plan)]
        try:
            args += ["--startband", f"{float(r0['pmin']):g},{float(r0['pmax']):g}"]
        except (KeyError, ValueError):
            pass
        xt = Path(str(out_base) + ".xtrack.txt")
        with xt.open("w", newline="", encoding="utf-8") as f:
            f.write("tick,x\n")
            for t in sorted(replay_rows):
                f.write(f"{t},{float(replay_rows[t]['x']):.4f}\n")
        args += ["--xtrack", str(xt)]
    return args


def model_argv(level_dir: Path, plan: Path, out_base: Path, leveldp: Path,
               level: int, replay_rows: dict[int, dict] | None = None) -> list[str] | None:
    """leveldp's replay of `plan`, built the way fidelity_diff.model_replay builds it, from the
    lab's dumps for an official level or from the dumps the check's own replay wrote (a custom
    level also gets custom_start_args)."""
    from fidelity_diff import groups_args, whole_run_args
    official = (LEVEL_DATA / f"objrects_lv{level}.txt").exists()
    if official:
        objrects = LEVEL_DATA / f"objrects_lv{level}.txt"
        trig = LEVEL_DATA / f"triggers_lv{level}.txt"
        grp = LEVEL_DATA / f"objgroups_lv{level}.txt"
        obb = LEVEL_DATA / f"obb_lv{level}.txt"
    else:
        objrects = level_dir / "objrects.txt"
        trig, grp, obb = (level_dir / "triggers.txt", level_dir / "objgroups.txt",
                          level_dir / "obb.txt")
    if not objrects.exists():
        return None
    args = [str(objrects), "--replay", str(plan), "--out", str(out_base)]
    if trig.exists() and grp.exists():
        args += ["--triggers", str(trig), "--objgroups", str(grp)]
    if obb.exists():
        args += ["--obb", str(obb)]
    args += groups_args(plan)
    args += whole_run_args(level)
    if not official:
        args += custom_start_args(level_dir, plan, out_base, replay_rows or {})
    return [str(leveldp)] + args


def cmd_check(a) -> int:
    from gdmcp.data import diff_trace
    from fidelity_diff import record_provenance
    run = Path(a.run)
    meta = json.loads((run / "run.json").read_text(encoding="utf-8"))
    tk = run / "takeover"
    start, cps, attempts = parse_summary(tk / "summary.txt")
    if not start:
        print(f"no start line in {tk / 'summary.txt'}")
        return 1
    T = int(start["t"])
    dump = read_dump(run / "dump.csv")
    level = int(meta["level"])
    data_root = _WorkerBase(a.worker_id, Path(a.workers_root)).data
    level_dir = run / "level"
    official = (LEVEL_DATA / f"objrects_lv{level}.txt").exists()
    results = []
    for att in attempts:
        name = att.get("file", "")
        plan = tk / name
        if not plan.exists():
            continue
        n = int(att["attempt"])
        stem = plan.name.replace(".plan.txt", "")
        out: dict = {"attempt": n, "outcome": att.get("outcome"), "tick": int(att.get("tick", -1)),
                     "x": float(att.get("x", 0)), "presses": int(att.get("presses", 0)),
                     "other_presses": int(att.get("other", 0)),
                     "checkpoint": int(att.get("cp", 0)), "from_tick": int(att.get("from", -1))}
        # 1. the attempt flown again from the head, headless
        if meta.get("levelfile"):
            data_root.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(run / meta["levelfile"], data_root / meta["levelfile"])
        want_dumps = not official and not ((level_dir / "objrects.txt").exists()
                                           and (level_dir / "levelsettings.txt").exists())
        cfg = replay_cfg(meta, data_root, plan, want_dumps)
        res = run_session(a.worker_id, cfg, timeout_s=a.timeout_minutes * 60,
                          workers_root=Path(a.workers_root), mod_file=Path(a.mod))
        clear, detail = R.clear_verdict(res.lines)
        out["replay_outcome"] = "clear" if clear else detail
        rdump = tk / f"{stem}.replay.dump.csv"
        copy_held_file(res.data_root / "dump.csv", rdump)
        gt = R.gt_last(res.lines, None)
        if gt and gt.get("rows", 0) > 0:
            copy_held_file(res.data_root / "grouptrace_last.txt",
                           Path(str(plan) + ".groups.live.txt"))
        if want_dumps:
            level_dir.mkdir(exist_ok=True)
            for f in LEVEL_DUMPS:
                copy_held_file(res.data_root / f, level_dir / f)
        if not official and level_dir.exists():
            # a run checked before these were kept gets them from this replay
            for f, _ in LEVEL_DUMPS_OPTIONAL:
                if not (level_dir / f).exists() and (res.data_root / f).exists():
                    copy_held_file(res.data_root / f, level_dir / f)
        rec = [l for l in res.lines if l.startswith("level record changed:")]
        out["replay_record_line"] = rec[-1] if rec else "(missing)"
        replay_rows = max(read_dump(rdump).values(), key=len, default={})
        sess = session_rows(dump, cps, att)
        out["replay_check"] = compare_runs(sess, replay_rows, out["tick"], end_tick(res.lines))
        # 2. the model against the headless replay
        if not a.no_model:
            base = tk / f"{stem}.model"
            argv = model_argv(level_dir, plan, base, Path(a.leveldp), level, replay_rows)
            objrects = Path(argv[1]) if argv else None
            if argv is None:
                out["model"] = {"status": "no level dumps"}
            elif has_grouped_colliders(objrects) and "--groups" not in argv:
                out["model"] = {"status": "NO-GROUPS (moving geometry, no recording)"}
            else:
                r = subprocess.run(argv, stdout=subprocess.PIPE, text=True, errors="replace")
                trace = Path(str(base) + ".trace.csv")
                m = re.search(r"REPLAY: model DIED at t=(\d+)", r.stdout)
                record_provenance(trace, argv, level=level, died=int(m.group(1)) if m else -1,
                                  exit_code=r.returncode, whole_run=True, with_fixups=False,
                                  with_fixups_requested=False)
                d = diff_trace(trace, rdump, tol=a.tol, limit=10 ** 9)
                if "error" in d:
                    out["model"] = {"status": "ERROR", "note": d["error"]}
                else:
                    first_t = d.get("first_divergence")   # a tick, absent when they agree
                    row0 = dict(zip(d["cols"], d["rows"][0])) if d.get("rows") else {}
                    out["model"] = {
                        "status": "OK", "model_died": int(m.group(1)) if m else -1,
                        "first_divergence": first_t, "first_row": row0,
                        "n_div": len(d.get("rows", [])), "common": d.get("common"),
                        "side": (None if first_t is None
                                 else "plan (before the point)" if first_t <= T
                                 else "person (after the point)"),
                        "per_half": d.get("per_half", {}),
                    }
        results.append(out)

    report = {"run": str(run), "level": level, "point_tick": T, "start": start,
              "attempts": results}
    (run / "check.json").write_text(json.dumps(report, indent=2, default=str), encoding="utf-8")
    print(f"\nlv{level}  point t={T}  ({len(results)} attempts)   -> {run / 'check.json'}")
    print(f"{'att':>4} {'outcome':>8} {'end t':>7} {'press':>5}  {'replay':<34} model")
    for r in results:
        rc = r["replay_check"]
        fp = rc["first_physics_difference"]
        # An attempt the end key cut short has no end of its own to agree on.
        if fp is not None:
            same = "PARTS t=%s %s" % (fp["tick"], fp["column"])
        elif rc["session_end"] == rc["replay_end"] or r["outcome"] == "ended":
            same = "same to t=%s" % rc["session_end"]
        else:
            same = "ends differ (%s vs %s)" % (rc["session_end"], rc["replay_end"])
        mdl = r.get("model", {})
        if mdl.get("status") == "OK":
            fd = mdl.get("first_divergence")
            row = mdl.get("first_row", {})
            md = (f"first parting t={fd} x={row.get('x')} {row.get('gd_mode', '')} "
                  f"dy={row.get('dy')} dvy={row.get('dvy')} ({mdl['side']})"
                  if fd is not None else "no parting over tol")
        else:
            md = mdl.get("status", "-")
        print(f"{r['attempt']:>4} {r['outcome'] or '':>8} {r['tick']:>7} {r['presses']:>5}  "
              f"{same:<34} {md}")
    rec = [l for l in R.read_lines(run / "result.txt") if l.startswith("level record changed:")]
    print("takeover session: " + (rec[-1] if rec else "WARNING: no `level record changed:` line"))
    return 0


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    sub = ap.add_subparsers(dest="cmd", required=True)

    def common(p):
        # Named every time: which workers are free is the operator's arrangement, and a default
        # would sooner or later land on one a resident session holds.
        p.add_argument("--worker-id", type=int, required=True)
        p.add_argument("--workers-root", default=str(WORKERS_ROOT))
        p.add_argument("--mod", default=str(BUILD_MOD))
        p.add_argument("--leveldp", default=str(LEVELDP_EXE))
        p.add_argument("--timeout-minutes", type=float, default=10.0)
        p.add_argument("--tol", type=float, default=0.3)

    p = sub.add_parser("play", help="fly a plan to a point and hand the input to a person")
    p.add_argument("--level", type=int, required=True,
                   help="the level number (with --levelfile, the id the session reports)")
    p.add_argument("--levelfile", default="", help="a custom level's raw level file")
    p.add_argument("--levelfile-main", action="store_true",
                   help="load --levelfile as a main level (levelfiletype=main): the spin-offs")
    p.add_argument("--plan", required=True, help="the plan flown up to the point (input= lines)")
    # Neither: the plan plays as a replay and the takeover key (F8) hands over where it stands.
    g = p.add_mutually_exclusive_group(required=False)
    g.add_argument("--at", type=int, help="hand over at this tick")
    g.add_argument("--at-x", type=float, help="hand over once player 1's x passes this")
    p.add_argument("--wait", type=float, default=1.0,
                   help="seconds of stop before each run from the checkpoint")
    p.add_argument("--slowmo", type=int, default=1,
                   help="play at 1/N speed (dt only; the ticks and the record do not change)")
    p.add_argument("--resolution-index", type=int, default=25)
    p.add_argument("--out", default=str(DEFAULT_OUT))
    p.add_argument("--no-check", action="store_true", help="collect only; run check later")
    common(p)

    c = sub.add_parser("check", help="replay and model-compare every attempt of a run")
    c.add_argument("run", help="the run directory play wrote")
    c.add_argument("--no-model", action="store_true")
    common(c)

    a = ap.parse_args(argv)
    return cmd_play(a) if a.cmd == "play" else cmd_check(a)


if __name__ == "__main__":
    sys.exit(main())
