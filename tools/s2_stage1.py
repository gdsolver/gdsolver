"""S2 stage 1: the model and the self-running shadow walked over the same captured window, both against GD.

    python tools/s2_stage1.py --self-test <self_test exe> --leveldp <leveldp exe> --out <dir> [--jobs N] [--only NAME ...]

Offline only: nothing here starts the game. Each window is a capture under widecap8 (the 10-29 status file
lists them and the --from / --to self_test takes). For each window:

  shadow  tools/self_test.cpp as the status file runs it, with ST_STATE=<file>: the shadow's state after the buttons
          of every step (atButtons) and at the pass's exit. Seeded from the first btraw head image (tick F), fed the
          btraw button edges and the wtrace world (exact positions, wtlast, the toggle one row behind), no events file.
  model   leveldp --replay of the capture's own plan (autorun.cfg's input= lines, the plan GD ran) anchored with
          --start at the dump row of tick F (quick_regress.start_fields, the instruments' anchor), --startband,
          --startgmod, GD's x up to F (--xtrack), the pads / rings result.txt says GD fired before F (--spentpad /
          --spentorb), the window's level dumps, and the world converted from the same wtrace.csv into --groups
          rows (tick,uid,cx,cy,w,h,on,rot at %.9g, a hold row before every change so the model's linear
          interpolation between rows cannot move a resting object). No --triggers (the captures have no
          objgroups.txt) and no --anchor-state payload.

Both are compared with GD's dump.csv (attempt 1): the dump row of tick t is the player after the buttons of step t,
which is the shadow's atButtons(t) (checked: x, y, vy bit-equal on the windows the shadow matches), and the model
trace's row t (the fidelity tools' convention). Per arm: first tick with |dx| or |dy| > 0.01 (and the field), the
first tick off by more than 0.001 in x / y (the print precision of the two texts), max |dy| and |dvy| while both GD and the arm are alive, the first
mode / gravity / size disagreement, the arm's death tick against GD's. Ticks compared: F+1 .. the window's end.
"""
from __future__ import annotations

import argparse
import csv
import json
import os
import re
import subprocess
import sys
from concurrent.futures import ProcessPoolExecutor
from functools import partial
from pathlib import Path

_HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(_HERE.parent / "py"))
from quick_regress import start_fields  # noqa: E402
from gdtas.solveutil import MODE_ID  # noqa: E402
from gdtas.paths import LAB  # noqa: E402

STATUS = LAB / "notes" / "selfshadow-window-status-1029.txt"
CAP8 = LAB / "tmp" / "widecap8"
CAP9 = LAB / "tmp" / "widecap9"
# --dual: the five player-2 captures (cfg btrawp2=1 / ctrawp2=1, 10-31), each with the range it was taken for. The shadow arm runs with ST_DUAL=1 (self_test
# steps and compares player 2), and the model arm is compared with the dump's player-2 columns as well.
DUAL_WINDOWS = (("o_lv16_13210", 13210, 13510), ("o_lv20_17270", 17270, 17870), ("s_116791791_9298_10900", 9298, 10900),
                ("a_113256289_18100_20700", 18100, 20700), ("c_119550490_4800_5110", 4800, 5110))
OPTIONAL = (("levelsettings.txt", "--levelsettings"), ("rotgameplay.txt", "--rotgameplay"),
            ("forceblocks.txt", "--forceids"), ("areas.txt", "--areas"), ("items.txt", "--items"),
            ("obb.txt", "--obb"))
MODE_NAME = {v: k for k, v in MODE_ID.items()}
MODE_NAME[0] = "cube"
TOL = 0.01
ANY = 0.001


def windows():
    for ln in STATUS.read_text(encoding="utf-8").splitlines()[1:]:
        c = ln.split("\t")
        if len(c) < 3:
            continue
        name, st, set_ = c[0], c[2], c[-1]
        m = re.search(r"ctraw steps (-?\d+)\.\.(-?\d+)", st)
        if not m:
            continue
        yield {"name": name, "set": set_, "from": int(m.group(1)), "to": int(m.group(2)),
               "pass_range": set_ != "widecap4", "status1029": st.split(" (")[0].replace("status: ", "")}


def read_dump(path: Path, attempt: str = "1") -> dict[int, dict]:
    rows = {}
    with path.open(newline="", encoding="utf-8", errors="replace") as f:
        for r in csv.DictReader(f):
            if r.get("attempt") == attempt:
                rows[int(r["tick"])] = r
    return rows


def write_plan(win: Path, dst: Path) -> int:
    lines = [ln.strip() for ln in (win / "autorun.cfg").read_text(encoding="utf-8", errors="replace").splitlines()
             if ln.strip().startswith("input=")]
    dst.write_text("\n".join(lines) + "\n", encoding="ascii")
    return len(lines)


def write_groups(win: Path, dst: Path, end_tick: int) -> dict:
    """wtrace(_last).csv -> leveldp --groups rows, exact. cx/cy as self_test places the rect: the node position
    shifted by (objrects centre - the first wtrace position)."""
    base = {}
    with (win / "objrects.txt").open(newline="", encoding="utf-8", errors="replace") as f:
        for r in csv.DictReader(f):
            try:
                base[int(float(r["uid"]))] = (float(r["cx"]), float(r["cy"]), float(r["w"]), float(r["h"]))
            except (KeyError, ValueError):
                pass
    wsrc = win / "wtrace_last.csv" if (win / "wtrace_last.csv").exists() else win / "wtrace.csv"
    if not wsrc.exists():   # nothing tracked moved in this capture: an empty recording, as the shadow has no movers
        dst.write_text(f"tick,uid,cx,cy,w,h,on,rot\nend,{end_tick}\n", encoding="ascii")
        return {"src": None, "objects": 0, "rows": 0, "hold_rows": 0}
    # streamed (a capture's wtrace is up to 1 GB) and cut at end_tick: the recorder writes the rows in tick order
    r0: dict[int, tuple] = {}
    prev: dict[int, tuple] = {}
    n_rows = n_hold = 0
    with wsrc.open(newline="", encoding="utf-8", errors="replace") as f, dst.open("w", encoding="ascii", newline="\n") as o:
        o.write("tick,uid,cx,cy,w,h,on,rot\n")
        f.readline()
        for line in f:
            v = line.rstrip("\r\n").split(",")
            if len(v) < 6:
                continue
            t, uid = int(v[0]), int(v[1])
            if t > end_tick:
                break
            b = base.get(uid)
            if b is None:
                continue
            bcx, bcy, bw, bh = b
            x, y, rot, on = float(v[2]), float(v[3]), float(v[4]), int(v[5])
            if uid not in r0:
                r0[uid] = (x, y)
            w = float(v[6]) if len(v) > 7 and v[6] != "" else 0.0
            h = float(v[7]) if len(v) > 7 and v[7] != "" else 0.0
            if not (w > 0 and h > 0):
                w, h = bw, bh
            row = (x + (bcx - r0[uid][0]), y + (bcy - r0[uid][1]), w, h, on, rot)
            pv = prev.get(uid)
            if pv is not None and t > pv[0] + 1:
                p = pv[1]
                o.write(f"{t - 1},{uid},{p[0]:.9g},{p[1]:.9g},{p[2]:.9g},{p[3]:.9g},{p[4]},{p[5]:.9g}\n")
                n_hold += 1
            o.write(f"{t},{uid},{row[0]:.9g},{row[1]:.9g},{row[2]:.9g},{row[3]:.9g},{row[4]},{row[5]:.9g}\n")
            n_rows += 1
            prev[uid] = (t, row)
        o.write(f"end,{end_tick}\n")
    n_obj = len(r0)
    return {"src": wsrc.name, "objects": n_obj, "rows": n_rows, "hold_rows": n_hold}


def fired_before(win: Path, key: str, t0: int, label: str = "map") -> list[int]:
    """The uids result.txt's `<key> <label>:` line (uid:tick, the mod's activation hooks) has before t0 -- `< t0`, as
    the loop's spentPadArg / spentOrbArg / portalPayload read the same ticks."""
    txt = (win / "result.txt").read_text(encoding="utf-8", errors="replace")
    m = re.search(rf"^{key} {label}: ([^\n(]*)", txt, re.M)
    if not m:
        return []
    out = []
    for tok in m.group(1).split(","):
        tok = tok.strip()
        if ":" not in tok:
            continue
        u, t = tok.split(":", 1)
        try:
            if int(t) < t0:
                out.append(int(u))
        except ValueError:
            pass
    return sorted(set(out))


def write_shadow_seed(win: Path, F: int, dst: Path) -> bool:
    """--shadowseedfile for leveldp --shadowstep (dp/shadow_step.hpp): the btraw head row of step F (self_test's choice of head row), F's button-1 edges,
    and ctraw's layer words at F -- what self_test seeds its shadow with."""
    snap, press, release, head_fn = None, 0, 0, None
    with (win / "btraw.csv").open(newline="", encoding="utf-8", errors="replace") as f:
        f.readline()
        for line in f:
            c = line.rstrip("\r\n").split(",")
            if len(c) < 8 or c[2] != "1":
                continue
            label, fn, arg = int(c[1]), int(c[3]), int(c[4])
            if fn in (3, 4) or (fn in (1, 2) and arg != 1):
                continue
            tick = label if fn == 5 else label + 1
            if tick != F:
                continue
            img = c[6].split(";")[0]
            if (snap is None or (fn != 5 and head_fn == 5)) and len(img) >= 2 * 0xc80 and not (snap is not None and head_fn != 5):
                snap, head_fn = c[6], fn
            if fn == 1 and arg == 1:
                press = 1
            elif fn == 2 and arg == 1:
                release = 1
    if snap is None:
        return False
    layer = None
    import struct
    with (win / "ctraw.csv").open(newline="", encoding="utf-8", errors="replace") as f:
        f.readline()
        for line in f:
            v = line.rstrip("\r\n").split(",")
            if len(v) < 8 or v[2] != "1" or int(v[1]) != F:
                continue
            parts = v[6].split(";")
            if len(parts) >= 8:
                A, B, C, E = bytes.fromhex(parts[2]), bytes.fromhex(parts[3]), bytes.fromhex(parts[4]), bytes.fromhex(parts[6])
                f32 = lambda b, o: struct.unpack_from("<f", b, o)[0]
                mx, mn = parts[7].split(":")
                layer = ",".join(str(x) for x in (B[0x11], B[0x122], B[0x10], B[0x12], repr(f32(B, 8)), repr(f32(C, 4)), repr(f32(A, 8)),
                                                  B[0x118] if len(B) > 0x118 else 0, repr(f32(B, 0x11c)) if len(B) >= 0x120 else 0, mn, mx,
                                                  repr(f32(E, 8))))
            break
    snap2, head2 = None, None   # player 2's head row of step F (btraw who = 2, the same choice), when the capture has one
    with (win / "btraw.csv").open(newline="", encoding="utf-8", errors="replace") as f:
        f.readline()
        for line in f:
            c = line.rstrip("\r\n").split(",")
            if len(c) < 8 or c[2] != "2":
                continue
            label, fn, arg = int(c[1]), int(c[3]), int(c[4])
            if fn in (3, 4) or (fn in (1, 2) and arg != 1):
                continue
            if (label if fn == 5 else label + 1) != F:
                continue
            img = c[6].split(";")[0]
            if (snap2 is None or (fn != 5 and head2 == 5)) and len(img) >= 2 * 0xc80 and not (snap2 is not None and head2 != 5):
                snap2, head2 = c[6], fn
    txt = f"tick={F}\nsnap={snap}\npress={press}\nrelease={release}\n" + (f"layer={layer}\n" if layer else "") + (f"snap2={snap2}\n" if snap2 else "")
    dst.write_text(txt, encoding="ascii")
    return True


def gd_death(win: Path) -> int:
    m = re.search(r"^death: attempt=1 tick=(\d+)", (win / "result.txt").read_text(encoding="utf-8", errors="replace"), re.M)
    return int(m.group(1)) if m else -1


def fnum(s, d=0.0):
    try:
        return float(s)
    except (TypeError, ValueError):
        return d


def compare(gd: dict, arm: dict, ticks: list[int], gd_dead_t: int, arm_dead_t: int) -> dict:
    """arm[t] = (x, y, vy, mode, up, mini)."""
    first_off = None
    first_any = None
    first_mode = first_up = first_mini = None
    max_dy = max_dvy = max_dx = 0.0
    n = 0
    for t in ticks:
        g, a = gd.get(t), arm.get(t)
        if g is None or a is None:
            continue
        if gd_dead_t >= 0 and t > gd_dead_t:
            break
        if arm_dead_t >= 0 and t > arm_dead_t:
            break
        n += 1
        gx, gy, gv = fnum(g["x"]), fnum(g["y"]), fnum(g["yvel"])
        dx, dy, dv = abs(a[0] - gx), abs(a[1] - gy), abs(a[2] - gv)
        if first_any is None and (dx > ANY or dy > ANY):
            # the dump prints %.9g and the model trace %.10g: below ANY the two texts can differ for the same float
            first_any = (t, "x" if dx > ANY else "y")
        if first_off is None and (dx > TOL or dy > TOL):
            first_off = (t, "x" if dx > TOL else "y", round(a[0] - gx, 4), round(a[1] - gy, 4))
        max_dx, max_dy, max_dvy = max(max_dx, dx), max(max_dy, dy), max(max_dvy, dv)
        gmode = g["mode"]
        if first_mode is None and a[3] != gmode:
            first_mode = (t, a[3], gmode)
        if first_up is None and int(a[4]) != (1 if g["upsideDown"] == "1" else 0):
            first_up = t
        gmini = 1 if fnum(g["vsize"], 1.0) < 0.9 else 0
        if first_mini is None and int(a[5]) != gmini:
            first_mini = t
    return {"compared": n, "first_off": first_off, "first_any": first_any, "max_dx": max_dx, "max_dy": max_dy,
            "max_dvy": max_dvy, "first_mode": first_mode, "first_up": first_up, "first_mini": first_mini,
            "died": arm_dead_t}


def run_window(w: dict, a) -> dict:
    name = w["name"]
    win = (CAP9 if w["set"] == "widecap9" else CAP8) / name
    out = Path(a.out) / name
    out.mkdir(parents=True, exist_ok=True)
    res = {"name": name, "set": w["set"], "from": w["from"], "to": w["to"], "status1029": w["status1029"]}
    if not (win / "btraw.csv").exists():
        res["error"] = "no btraw.csv"
        return res
    # ---- shadow
    st_csv = out / "shadow.csv"
    args = [a.self_test, str(win / "btraw.csv"), str(win / "objrects.txt"), str(win / "ctraw.csv")]
    if w["pass_range"]:
        args += ["--from", str(w["from"]), "--to", str(w["to"])]
    env = dict(os.environ)
    for k in list(env):
        if k.startswith("ST_"):
            del env[k]
    env["ST_STATE"] = str(st_csv)
    if w["set"] == "widecap9":
        env["ST_DUAL"] = "1"
    for kv in a.shadow_env or []:   # e.g. ST_LAYERFREEZE=1: the shadow's layer words from ctraw at tick F only
        k, _, v = kv.partition("=")
        env[k] = v
    res["shadow_env"] = a.shadow_env or []
    reuse = Path(a.reuse_shadow) / name if a.reuse_shadow else None
    if reuse is not None and (reuse / "shadow.csv").exists() and (reuse / "self_test.out").exists():
        # the shadow arm of an earlier run with the same self_test (only the model arm changes between --model-flag runs)
        import shutil
        shutil.copyfile(reuse / "shadow.csv", st_csv)
        out_txt = (reuse / "self_test.out").read_bytes()
        (out / "self_test.out").write_bytes(out_txt)
        res["shadow_reused_from"] = str(reuse)
        p = subprocess.CompletedProcess(args, 0, out_txt, b"")
    else:
        p = subprocess.run(args, env=env, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        (out / "self_test.out").write_bytes(p.stdout)
        (out / "self_test.err").write_bytes(p.stderr)
    res["self_test_argv"] = args[1:]
    m = re.search(r"^status: (\w+)", p.stdout.decode("utf-8", "replace"), re.M)
    res["self_test_status"] = m.group(1) if m else "?"
    for key in ("p2status", "p2bytes", "p2phys"):   # self_test under ST_DUAL: player 2 against the game's player-2 rows
        m2 = re.search(rf"^{key}: (.*)$", p.stdout.decode("utf-8", "replace"), re.M)
        if m2:
            res["shadow_" + key] = m2.group(1)
    if not st_csv.exists() or st_csv.stat().st_size == 0:
        res["error"] = "shadow wrote no state (" + res["self_test_status"] + ")"
        return res
    shadow, sh_dead = {}, -1
    first_tick = None
    with st_csv.open(newline="") as f:
        for r in csv.DictReader(f):
            t = int(r["tick"])
            if first_tick is None:
                first_tick = t
            shadow[t] = (float(r["bx"]), float(r["by"]), float(r["bvy"]), r["mode"], int(r["upside"]),
                         1 if float(r["size"]) < 0.9 else 0)
            if sh_dead < 0 and r["died"] == "1":
                sh_dead = t
    F = first_tick
    res["seed_tick"] = F
    # ---- GD
    gd = read_dump(win / "dump.csv")
    gdd = gd_death(win)
    res["gd_death"] = gdd
    # ---- model
    plan = out / "plan.txt"
    res["plan_edges"] = write_plan(win, plan)
    r0 = gd.get(F)
    if r0 is None:
        res["error"] = f"no dump row at the seed tick {F}"
        return res
    # the world up to the window's end (+2): the model's replay runs on, but nothing past the window is compared
    # one world file per worker process, overwritten by its next window (a full run's worlds came to ~7 GB when
    # each window kept its own): disk stays at <jobs> files. --keep-world keeps the old per-window file.
    if a.keep_world:
        world = out / "world.groups.txt"
    else:
        wd = Path(a.world_dir) if a.world_dir else Path(a.out) / "_world"
        wd.mkdir(parents=True, exist_ok=True)
        # --world-dir: one file per worker slot (0 .. jobs-1), shared by every run that names the dir, so repeated runs
        # overwrite the same few files instead of leaving <jobs> new ones each
        world = wd / (f"slot{_SLOT}.groups.txt" if a.world_dir and _SLOT is not None else f"{os.getpid()}.groups.txt")
    res["groups"] = write_groups(win, world, w["to"] + 2)
    res["groups"]["bytes"] = world.stat().st_size
    xt = out / "xtrack.txt"
    xt.write_text("tick,x\n" + "".join(f"{t},{float(gd[t]['x']):.4f}\n" for t in sorted(gd) if t <= F), encoding="ascii")
    margs = [str(win / "objrects.txt"), "--replay", str(plan), "--out", str(out / "model"),
             "--start", start_fields(F, r0, plan, gd.get(F - 1), gd)]
    if r0.get("pmin") and r0.get("pmax"):
        margs += ["--startband", f"{r0['pmin']},{r0['pmax']}"]
    if r0.get("gravityMod"):
        margs += ["--startgmod", r0["gravityMod"]]
    for fn, flag in OPTIONAL:
        if (win / fn).exists():
            margs += [flag, str(win / fn)]
    margs += ["--groups", str(world), "--xtrack", str(xt)]
    sp = fired_before(win, "padseed", F)
    so = fired_before(win, "ringseed", F)
    if sp:
        margs += ["--spentpad", ",".join(map(str, sp))]
    if so:
        margs += ["--spentorb", ",".join(map(str, so))]
    # the loop's anchor payload, the part this capture can give: the gravity portals GD had spent by F (cfg
    # portalpayload, on in the loop). Not given: the touch payload (off in the loop) and the hist payload.
    p1 = fired_before(win, "portalseed", F)
    p2 = fired_before(win, "portalseed", F, "map2")
    if p1 or p2:
        margs += ["--anchor-state", "owns=portal;portal=" + ",".join(map(str, p1)) + ";portal2=" + ",".join(map(str, p2))]
    if a.shadow_seed:   # leveldp --shadowstep: the shadow's first image is the game's at F, as self_test's
        sf = out / "shadowseed.txt"
        res["shadow_seed_written"] = write_shadow_seed(win, F, sf)
        if res["shadow_seed_written"]:
            margs += ["--shadowseedfile", str(sf)]
    margs += a.model_flag or []
    res["model_argv"] = margs
    import time as _time
    t_model = _time.perf_counter()
    p = subprocess.run([a.leveldp] + margs, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    res["model_seconds"] = round(_time.perf_counter() - t_model, 3)
    (out / "model.log").write_bytes(p.stdout)
    so_txt = p.stdout.decode("utf-8", "replace")
    mst = re.search(r"^shadowstep: eligible .*$", so_txt, re.M)
    res["shadowstep"] = mst.group(0) if mst else None
    m = re.search(r"REPLAY: model DIED at t=(\d+)", so_txt)
    # the replay names the row after the dying step (measured on the death windows: GD and the shadow die at D,
    # "model DIED at t=D+1" with the trace row D+1 its last); the death on GD's tick is that minus one
    md_raw = int(m.group(1)) if m else -1
    md = md_raw - 1 if md_raw >= 0 else -1
    res["model_died_raw"] = md_raw
    mc = re.search(r"REPLAY: cause=(\S+)(?: obj=uid(\d+))?", so_txt)
    res["model_cause"] = mc.group(0) if mc else None
    mg = re.search(r"^groups: (\d+) samples for (\d+) objects", so_txt, re.M)
    res["model_groups_loaded"] = mg.group(0) if mg else None
    res["model_exit"] = p.returncode
    model = {}
    model2 = {}   # player 2: (dual, y2, vy2, flip2) of the trace
    tr = Path(str(out / "model") + ".trace.csv")
    if tr.exists():
        with tr.open(newline="") as f:
            for r in csv.DictReader(f):
                t = int(r["tick"])
                frame = int(fnum(r.get("frame"), 0))
                up = int(fnum(r.get("flip"), 0))
                if frame == 3:
                    up = 1 - up
                model[t] = (fnum(r["x"]), fnum(r["y"]), fnum(r["vy"]), MODE_NAME.get(int(fnum(r["mode"])), "?"), up,
                            int(fnum(r.get("mini"), 0)))
                model2[t] = (int(fnum(r.get("dual"), 0)), fnum(r.get("y2")), fnum(r.get("vy2")), int(fnum(r.get("flip2"), 0)))
    else:
        res["error"] = "model wrote no trace"
    ticks = list(range(F + 1, w["to"] + 1))
    res["shadow"] = compare(gd, shadow, ticks, gdd, sh_dead)
    res["model"] = compare(gd, model, ticks, gdd, md)
    if w["set"] == "widecap9":   # player 2: the trace's dual / y2 / flip2 against the dump's dual / p2y / p2up
        n2, first2, maxdy2 = 0, None, 0.0
        for t in ticks:
            g, m2 = gd.get(t), model2.get(t)
            if g is None or m2 is None or (gdd >= 0 and t > gdd) or (md >= 0 and t > md):
                continue
            gdual = 1 if g.get("dual") == "1" else 0
            if gdual != m2[0]:
                first2 = first2 or (t, "dual", m2[0], gdual)
                continue
            if not gdual:
                continue
            n2 += 1
            dy2 = abs(m2[1] - fnum(g.get("p2y")))
            maxdy2 = max(maxdy2, dy2)
            if first2 is None and (dy2 > TOL or m2[3] != (1 if g.get("p2up") == "1" else 0)):
                first2 = (t, "y2" if dy2 > TOL else "flip2", round(m2[1] - fnum(g.get("p2y")), 4), m2[3])
        res["model_p2"] = {"compared_dual_ticks": n2, "first_off": first2, "max_dy2": maxdy2}
    # a death past the window's end is not the window's (the model's replay runs on to the level's end)
    for arm in ("shadow", "model"):
        d = res[arm]["died"]
        res[arm]["died_in_window"] = d if (d >= 0 and d <= w["to"]) else -1
        res[arm]["model_last_tick"] = max(model) if (arm == "model" and model) else None
    res["gd_dead_rows"] = [t for t in ticks if t in gd and gd[t].get("dead") == "1"][:3]
    # the seed itself: the shadow's atButtons(F) and the model's start against dump(F)
    res["seed_check"] = {"shadow": compare(gd, shadow, [F], -1, -1)["first_any"]}
    return res


_SLOT = None


def _init_slot(q) -> None:
    global _SLOT
    _SLOT = q.get()


def safe_run_window(w: dict, a) -> dict:
    try:
        return run_window(w, a)
    except Exception as e:  # noqa: BLE001 -- one window's failure is reported in its row, not the run's
        return {"name": w["name"], "set": w["set"], "from": w["from"], "to": w["to"], "error": f"{type(e).__name__}: {e}"}


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--self-test", required=True)
    ap.add_argument("--leveldp", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--jobs", type=int, default=4)
    ap.add_argument("--only", nargs="*")
    ap.add_argument("--dual", action="store_true", help="add the five widecap9 player-2 windows (DUAL_WINDOWS)")
    ap.add_argument("--shadow-env", action="append", help="KEY=VALUE for self_test, e.g. ST_LAYERFREEZE=1 (repeatable)")
    ap.add_argument("--keep-world", action="store_true", help="keep each window's world.groups.txt (up to ~7 GB a run)")
    ap.add_argument("--model-flag", action="append", help="an extra leveldp flag for the model arm, e.g. --model-flag=--shadowstep=cube (repeatable)")
    ap.add_argument("--shadow-seed", action="store_true", help="give the model arm --shadowseedfile: the btraw head image of the seed tick (for --shadowstep)")
    ap.add_argument("--world-dir", help="one reusable groups file per worker slot here, overwritten window after window and run after run")
    ap.add_argument("--reuse-shadow", help="a previous run dir: take each window's shadow.csv / self_test.out from it instead of running self_test")
    a = ap.parse_args()
    ws = list(windows())
    if a.dual:
        ws += [{"name": n, "set": "widecap9", "from": f0, "to": t0, "pass_range": True, "status1029": "-"} for n, f0, t0 in DUAL_WINDOWS]
    ws = [w for w in ws if not a.only or w["name"] in a.only]
    import multiprocessing as mp
    q = mp.Manager().Queue()
    for i in range(a.jobs):
        q.put(i)
    with ProcessPoolExecutor(a.jobs, initializer=_init_slot, initargs=(q,)) as ex:   # processes: the wtrace conversion is Python and holds the GIL
        res = list(ex.map(partial(safe_run_window, a=a), ws))
    Path(a.out).mkdir(parents=True, exist_ok=True)
    (Path(a.out) / "results.json").write_text(json.dumps(res, indent=1), encoding="utf-8")
    for r in res:
        s, m = r.get("shadow", {}), r.get("model", {})
        print(r["name"], r.get("error", ""), "F", r.get("seed_tick"), "| shadow", s.get("first_off"), s.get("died"),
              "| model", m.get("first_off"), m.get("died"), "| gd death", r.get("gd_death"))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
