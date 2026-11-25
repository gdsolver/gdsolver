"""The tools themselves. Both the MCP server and the CLI call in here.

A session (= one GD held open) is a module-level singleton. It is kept for as long as
the MCP server process lives, so the same GD can be reused across tool calls. This is
the one and only thing that never worked through PowerShell, and the biggest one.
"""

from __future__ import annotations

import importlib
import re
import time
from pathlib import Path

from . import data as D
from . import worker as W

# HOLD THESE AS MODULE REFERENCES (do not write `from .worker import Worker`).
# That is what lets gd_reload swap the two of them out with importlib.reload.
WorkerError = W.WorkerError

_session = None
_last: dict = {}     # result of the most recent gd_run (used by death_context)


def _need():
    if _session is None or not _session.is_alive():
        raise W.WorkerError("no session is open. Call gd_session_open first.")
    return _session


# A resident game does not keep growing (mcp/README.md, "Sweeps and the memory"): its private
# bytes settle within ~10 runs at 0.8-1.2 GB and stay. This is the safety net for what that
# measurement did not cover (a very long session, a level much larger than the ones measured), set
# at three times the largest value seen; the batch runner's own floor is free memory < 3 GB.
MAX_PRIVATE_MB = 4096.0


def _over_memory(w) -> bool:
    try:
        import psutil
        return psutil.Process(w.proc.pid).memory_info().private / 2**20 > MAX_PRIVATE_MB
    except Exception:       # a process that is gone or a missing psutil is not a reason to refuse
        return False


def parse_plan_file(path: str | Path) -> list[tuple[int, int]]:
    """Read a plan file in the `input=<tick>,<0|1>` format.

    Note: using a known solution as A SEED TO SOLVE FROM is forbidden (the absolute
    rule in CLAUDE.md). Reading a solution file here is only for "replaying a known
    run in order to observe it".

    ALWAYS READ WITH utf-8-sig. PowerShell's `Set-Content -Encoding utf8` adds a BOM,
    so reading as utf-8 turns the first line into "\\ufeffinput=...", it falls out of
    the `input=` test, and THE FIRST INPUT SILENTLY DISAPPEARS. A 2462-line file
    becomes 2461 inputs, and with nothing wrong in the log it shows up only as "only
    the first jump never fires" (actually hit on 2026-07-31; the MOD's loadInputsFile
    carries the same countermeasure).
    """
    out, skipped = [], 0
    for line in Path(path).read_text(encoding="utf-8-sig", errors="replace").splitlines():
        m = re.match(r"\s*input=(-?\d+)\s*,\s*(\d+)", line)
        if m:
            out.append((int(m.group(1)), 1 if int(m.group(2)) else 0))
        elif "input=" in line:
            skipped += 1
    # Do not drop them silently. The BOM case above burned an hour in the shape of
    # "one line quietly disappears"
    if skipped:
        raise ValueError(f"{path}: {skipped} lines contain `input=` but could "
                         f"not be parsed")
    out.sort(key=lambda p: p[0])
    return out


# ---------- session ----------

def gd_session_open(level: int, worker_id: int = W.MCP_WORKER, cfg: dict | None = None,
                    mod_file: str | None = None, fresh: bool = False) -> dict:
    """Launch one GD worker and hold it open (about 20 seconds).

    Every later gd_run only supplies a plan to this resident session, so it returns in
    a few seconds. cfg takes extra autorun.cfg keys (e.g. {"clearance": "1"}).

    A session already open on the same worker, running the same .geode and the same cfg
    (the level aside), is MOVED to the new level inside the running game instead of being
    relaunched (`switched` in the result): the mod leaves the level and enters the next one
    by the same path a multi-level cold run does. A run that reaches the end no longer costs
    a relaunch either (serverearm). fresh=True forces a new process, which is the arm to
    compare a result against; so does cfg {"serverearm": "0"}, the old behaviour.
    """
    global _session, _last
    cfg = {"serverearm": "1", **(cfg or {})}
    stale_objrects = None
    if (not fresh and _session is not None and _session.worker_id == worker_id
            and _session.is_alive()):
        stale_objrects = _session.data / "objrects.txt"
    elif _session is not None:
        _session.close()
    if stale_objrects is not None:
        # same hazard as below: the previous level's table must not be read for this one
        try:
            stale_objrects.unlink(missing_ok=True)
        except OSError:
            pass
        _last = {}
        return _session.switch(level, cfg, Path(mod_file or W.BUILD_MOD))
    if _session is not None:
        _session.close()
    _session = W.Worker(worker_id)
    _last = {}
    # A session's own objrects.txt KEEPS THE LEFTOVERS OF THE PREVIOUS SESSION
    # (measured 2026-08-17: lv17/lv16 queries read the lv22 table from 8/11, which
    # produced the misdiagnosis "a kill band with no solid object" and even an
    # unnecessary deadband). objects_path gives this file top priority, so always
    # delete it before opening, and let either a clearance=1 run rebuild it or the
    # fall-back to the main line's objrects_lv<N>.txt take over.
    try:
        stale = _session.data / "objrects.txt"
        if stale.exists():
            stale.unlink()
    except OSError:
        pass
    return dict(_session.open(level, cfg, Path(mod_file or W.BUILD_MOD)),
                switched=False)


def gd_session_close() -> dict:
    global _session, _last
    if _session is not None:
        _session.close()
        _session = None
    _last = {}
    return {"closed": True}


def gd_session_info() -> dict:
    if _session is None:
        return {"open": False}
    return dict(_session.info(), open=True, last_run=_last.get("summary"))


# ---------- runs ----------

def gd_run(plan: list | None = None, plan_file: str | None = None,
           inject: list | None = None, stop_at: int | None = None,
           timeout_s: float = 120.0, in_place: bool = False) -> dict:
    """Run one plan and return ONLY A SUMMARY.

    plan is [[tick, 0|1], ...]; plan_file is a file in the `input=t,d` format.
    What comes back is only outcome / tick / x / pct and the like. Call gd_trace next
    if you need the trajectory, gd_death_context for what surrounded the death.

    inject is [{"tick": T, "y": 150, "vy": 0, "x": 1234}, ...]. It overwrites the state
    after that tick's physics update, so it takes effect from the T+1 transition on.
    The point is that IT REMOVES THE NEED TO FIRST SOLVE A PLAN REACHING THAT STATE.
    Only position and y velocity can be injected (mode / size / gravity carry too much
    attendant state to stay consistent under a raw assignment). Injections that fired
    appear in `injected` in the return value - if one is missing, it was discarded
    because the run stepped past the tick.

    stop_at reports the state at that tick and ends the attempt (outcome="stopped").
    It closes "what happens if I run N ticks from this state" in a single call.

    in_place: by default a run on a level that has already run something first ENTERS THE
    LEVEL AGAIN inside the running game (about 2 s; `entered` says which happened), because
    that is the arrangement whose dump / trace / events / moving-geometry record were measured
    equal, row for row, to a relaunch (python -m gdmcp.equiv: 66 of 66 official cases and 21 of
    21 custom ones, against 64 and 13 for a reset in place). in_place=True resets the level where it stands instead (about 0.5 s). The
    physics columns of the dump and the trace agree in nearly every case, but GD's reset leaves
    some state as the last run had it: moving objects' cached rects at the first ticks (so the
    grouptrace record can differ), the flight band's pmin/pmax and snapdist until their first
    write, and in one measured case y by 2.5e-4. Use it for quick questions, not for a number
    you will compare against a relaunch.

    Note: a state built by injection is not necessarily "a state reachable from the
    start". Feed what you learn here back into the model, and ALWAYS VERIFY THE
    SOLUTION WITH A COLD RUN (CLAUDE.md).
    """
    global _last
    w = _need()
    entered = "as_opened"
    if _over_memory(w):
        w._reopen()
        entered = "relaunched (memory)"
    elif w.dirty:
        if in_place and w.rearm:
            entered = "reset_in_place"
        else:
            # The level has run something. Enter it afresh (~2 s, no relaunch) so this run starts
            # from what a launch gives; see the in_place note above.
            entered = "re-entered" if w.reenter().get("switched") else "relaunched"
    if plan_file:
        pairs = parse_plan_file(plan_file)
    elif plan:
        pairs = sorted(((int(t), 1 if int(d) else 0) for t, d in plan),
                       key=lambda p: p[0])
    else:
        pairs = []
    specs = [dict(s) for s in (inject or [])]
    r = w.run(pairs, timeout_s=timeout_s, injects=specs, stop_at=stop_at)
    summary = dict(r.as_dict(), n_inputs=len(pairs), entered=entered)
    if specs and len(r.injected) < len(specs):
        summary["warning"] = (f"{len(r.injected)} of {len(specs)} injections "
                              "fired. Some may have been dropped across a tick "
                              "(right after a checkpoint restore or a reset)")
    _last = {"summary": summary, "result": r}
    if r.outcome == "timeout":
        summary["note"] = ("cut off without either a death or a completion. The "
                           "plan may be too long, or the worker may be stuck "
                           "(check with gd_cmd('status'))")
    return summary


def gd_death_context(back_ticks: int = 40, x_pad: float = 120.0) -> dict:
    """Bundle the moments before the last run's death: state, events, nearby geometry."""
    w = _need()
    r = _last.get("result")
    if r is None:
        return {"error": "nothing has been run yet"}
    if r.outcome != "death":
        return {"error": f"the last run ended in {r.outcome}, so there is no "
                         f"death to give context for"}
    return D.death_context(w.data, w.level, r.tick, r.x, back_ticks, x_pad)


def gd_trace(t0: int = 0, t1: int | None = None, cols: list[str] | None = None,
             stride: int = 1, limit: int = 200) -> dict:
    """Return the per-tick state of the last run, with range, columns and stride."""
    return D.read_dump(_need().data / "dump.csv", t0, t1, cols, stride, limit)


def gd_events(t0: int = 0, t1: int | None = None,
              kinds: list[str] | None = None, limit: int = 200) -> dict:
    """Return the last run's events (trace.csv), narrowed by range and kind."""
    return D.read_trace(_need().data / "trace.csv", t0, t1, kinds, limit)


def gd_objects(x0: float | None = None, x1: float | None = None,
               y0: float | None = None, y1: float | None = None,
               types: list[int] | None = None, ids: list[int] | None = None,
               tick: int | None = None, pad: float = 60.0,
               limit: int = 300) -> dict:
    """Return the objects in an x range WITH HITBOXES AS MEASURED FROM GD.

    Passing tick looks at +/-pad around the x of that tick in the last run. That
    removes the chore of working x back from tick by hand (x ~ 1.29825*t).
    """
    w = _need()
    if tick is not None:
        d = D.read_dump(w.data / "dump.csv", tick, tick, cols=["tick", "x"],
                        limit=1)
        rows = d.get("rows") or []
        if not rows:
            return {"error": f"tick={tick} is not in the last run"}
        cx = float(rows[0][1])
        x0, x1 = cx - pad, cx + pad
    if x0 is None or x1 is None:
        return {"error": "either x0/x1 or tick is required"}
    return D.read_objects(D.objects_path(w.data, w.level), x0, x1, y0, y1,
                          types, ids, limit)


def gd_diff(trace_file: str, t0: int = 0, t1: int | None = None,
            tol: float = 0.3, limit: int = 8) -> dict:
    """Match the model trace against the last GD run; return THE FIRST DIVERGING TICK.

    trace_file is the `<plan>.trace.csv` leveldp emits (tick,x,y,vy,mode,grounded).
    This is the practice of "do not look at the death point; emit the same log on both
    sides and find the first tick that diverges", turned straight into a tool.

    tol is the tolerance in px / vy. 0.3 is the yardstick for a divergence that means
    something; lowering it to 0.001 picks up even float rounding.
    If they still match yet only GD finishes first, that is not a divergence - suspect
    A KILL TEST THE MODEL IS NOT APPLYING (hazard shape, clearance).

    `rows` are gated on the MAXIMUM of |dx|,|dy|,|dvy|,|dy2|,|dvy2|, so one number
    stands for both halves and three quantities and an improvement to one half
    alone cannot move it. `per_half` counts each quantity of each half against tol
    on its own; a p2 entry with cmp=0 means there was no second player to compare
    (read it as n/a, never as agreement).
    """
    return D.diff_trace(Path(trace_file), _need().data / "dump.csv",
                        t0, t1, tol, limit)


def gd_cmd(cmd: str, timeout_s: float = 10.0) -> dict:
    """Escape hatch to post a cmd.txt command directly (status / diag / pause / resume / step N)."""
    return {"cmd": cmd, "lines": _need().command(cmd, timeout_s)}

# ---------- gd_call: GD's own function on a scratch object ----------

_PACK = {"u8": "<B", "i8": "<b", "u16": "<H", "i16": "<h", "u32": "<I", "i32": "<i", "i64": "<q", "u64": "<Q", "f32": "<f", "f64": "<d"}


def _encode(p: dict) -> bytes:
    """A patch is {who: 'p'|'s', offset, hex} or {who, offset, type, value}."""
    import struct
    if "hex" in p:
        return bytes.fromhex(p["hex"])
    return struct.pack(_PACK[p["type"]], p["value"])


def _decode(blob: bytes, offset: int, typ: str):
    import struct
    fmt = _PACK[typ]
    return struct.unpack_from(fmt, blob, offset)[0]


def gd_call(function: str, patches: list | None = None, args: dict | None = None,
            read: list | None = None, timeout_s: float = 30.0) -> dict:
    """Call one of GD's own functions on a scratch object and return the bytes it leaves.

    The mod must run with cfg callverify=1 (gd_session_open(..., cfg={"callverify": "1"})).
    A NEW scratch PlayerObject (and, for the slope functions, a scratch slope GameObject) is made
    for every call; nothing a call leaves is read by the next, and nothing reaches the running
    level (the scratch player has no game layer, and is on no node).

    function:  one of "collidedWithSlopeInternal", "updateSlopeRotation".
    patches:   [{"who": "p", "offset": 0x9a8, "type": "f64", "value": 0.0}, {"who": "s", "offset": 0x4c, "hex": "00000000"}, ...]
               laid over the scratch player ("p") or slope object ("s") after they are made. Lay over
               plain data only (positions, velocities, flags, counters); a pointer slot is the
               scratch object's own and a patch over it is a crash.
    args:      {"dt": 0.25, "forced": 0}
    read:      [{"name": "launch", "who": "p", "offset": 0x9bc, "type": "f32"}, ...] decoded from the
               bytes after the call. The full bytes are returned as hex as well.
    """
    w = _need()
    lines = [f"fn {function}"]
    for k, v in (args or {}).items():
        if k == "rect":   # collidedWithObjectInternal: [x, y, w, h]
            lines.append("rect " + " ".join(repr(float(x)) for x in v))
        else:
            lines.append(f"arg {k} {v}")
    for p in (patches or []):
        lines.append(f"patch {p['who']} {int(p['offset'])} {_encode(p).hex()}")
    (w.data / "call_in.txt").write_text("\n".join(lines) + "\n", encoding="ascii")
    out_path = w.data / "call_out.txt"
    if out_path.exists():
        out_path.unlink()
    res = w.command("call", timeout_s)
    out = {"result_lines": res}
    # w.command returns on the first line any command adds to result.txt, which can be an earlier
    # command's late line: the answer is the file, so wait for it
    wait_until = time.time() + timeout_s
    def _written() -> bool:
        try:
            s = out_path.read_text(encoding="ascii")
        except OSError:
            return False
        return s.startswith("ok 0") or "sizes " in s

    while not _written() and time.time() < wait_until:
        time.sleep(0.1)
    if not out_path.exists():
        out["ok"] = False
        out["error"] = "no call_out.txt"
        return out
    ret: dict = {}
    for ln in out_path.read_text(encoding="ascii").splitlines():
        k, _, rest = ln.partition(" ")
        ret[k] = rest
    out["ok"] = ret.get("ok", "").startswith("1")
    for who in ("p", "s"):
        if f"ptr_slots_{who}" in ret:   # the 8-aligned offsets of the scratch object's heap pointers: patches never change these words
            out[f"ptr_slots_{who}"] = [int(x) for x in ret[f"ptr_slots_{who}"].split(",") if x]
    if not out["ok"]:
        out["error"] = ret.get("ok", "")
        return out
    blobs = {"p": bytes.fromhex(ret["player"]), "s": bytes.fromhex(ret["slope"]) if "slope" in ret else b""}
    out["sizes"] = ret.get("sizes")
    out["player_in_hex"] = ret.get("player_in")
    if "slope_in" in ret:
        out["slope_in_hex"] = ret["slope_in"]
    if "slope_hazard_off" in ret:
        out["slope_hazard_off"] = int(ret["slope_hazard_off"])
    out["player_hex"] = ret["player"]
    if "slope" in ret:
        out["slope_hex"] = ret["slope"]
    if "ret" in ret:
        out["ret"] = ret["ret"]
    if read:
        out["read"] = {r.get("name", f"{r['who']}+{r['offset']:#x}"): _decode(blobs[r["who"]], int(r["offset"]), r["type"]) for r in read}
    return out

def gd_reload() -> dict:
    """Reload the gdmcp implementation (fix things without restarting Claude Code).

    Limitation: THE BODY OF THIS FILE (tools.py) IS NOT SWAPPED OUT, because the
    function objects registered with MCP are the ones from startup. A restart is only
    needed when a tool's arguments or body change. Changes to worker.py / data.py are
    picked up by this. The session is closed (never mix instances of the old classes
    into the new code).
    """
    global _session, _last
    if _session is not None:
        _session.close()
    _session, _last = None, {}
    importlib.reload(D)
    importlib.reload(W)
    return {"reloaded": ["gdmcp.data", "gdmcp.worker"],
            "note": "changes to tools.py itself are not picked up (restart needed)"}


TOOLS = [gd_session_open, gd_session_close, gd_session_info, gd_run,
         gd_death_context, gd_trace, gd_events, gd_objects, gd_diff,
         gd_cmd, gd_call, gd_reload]
