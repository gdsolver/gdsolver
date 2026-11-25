"""Two s2_stage1.py runs side by side: the totals of each, the windows whose result changed, and for those the layer
words GD changed inside the window (from ctraw), i.e. what a shadow run with ST_LAYERFREEZE=1 was no longer given.

    python tools/s2_stage1_diff.py <run A (reference)> <run B> [--md out.md]

A window counts as changed when the shadow's first failure (tick or kind) or the verdict differs between A and B.
The model arm is checked to be the same in both (model.trace.csv byte-equal, same first failure). For each changed
window: the layer words self_test reads from ctraw (the same offsets as tools/self_test.cpp), their value at the
tick B's shadow took them (self_test.err's "layerfreeze: layer taken at t=" line) and every change after it up to
the window's end, with the tick of the first change of each word marked when it is at or before B's first failure.
"""
from __future__ import annotations

import argparse
import json
import struct
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from s2_stage1_table import first_fail, trigger_windows, INF  # noqa: E402
from s2_stage1 import CAP8  # noqa: E402

FIELDS = ("speedA", "speedB", "freeMode", "dual", "w764", "flipAmt", "f317c", "f317e", "scaleX", "scaleY", "ceilWord",
          "maxPortalY", "minPortalY")


def layer_of(pre: str):
    parts = pre.split(";")
    if len(parts) < 8:
        return None
    A, B, C, D, E = (bytes.fromhex(parts[i]) for i in range(2, 7))
    f32 = lambda b, o: struct.unpack_from("<f", b, o)[0]
    pq = parts[7].split(":")
    return {"speedA": f32(A, 8), "speedB": f32(A, 12), "freeMode": int(B[0x11] != 0), "dual": int(B[0x122] != 0),
            "w764": f32(C, 4), "flipAmt": f32(B, 0x11c) if len(B) >= 0x120 else 0.0,
            "f317c": int(D[0xc] != 0), "f317e": int(D[0xe] != 0),
            "scaleX": f32(E, 0), "scaleY": f32(E, 4), "ceilWord": f32(E, 8),
            "maxPortalY": float(pq[0]), "minPortalY": float(pq[1])}


def layer_changes(name: str, t0: int, to: int) -> tuple[dict, list]:
    """(the layer at t0, [(tick, field, old, new)] for t0 < tick <= to), player 1's rows."""
    base, prev, ch = None, None, []
    with (CAP8 / name / "ctraw.csv").open(encoding="utf-8", errors="replace") as f:
        f.readline()
        for line in f:
            v = line.split(",", 7)
            if len(v) < 8 or v[2] != "1":
                continue
            t = int(v[1])
            if t < t0:
                continue
            if t > to:
                break
            L = layer_of(v[6])
            if L is None:
                continue
            if base is None:
                base = prev = L
                continue
            for k in FIELDS:
                if L[k] != prev[k]:
                    ch.append((t, k, prev[k], L[k]))
            prev = L
    return base or {}, ch


def taken_at(run: Path, name: str):
    e = run / name / "self_test.err"
    if not e.exists():
        return None
    for ln in e.read_text(encoding="utf-8", errors="replace").splitlines():
        if ln.startswith("layerfreeze: layer taken at t="):
            return int(ln.rsplit("=", 1)[1])
    return None


def verdict(fs, fm):
    if fs == INF and fm == INF:
        return "both exact"
    if fs == fm:
        return "same tick"
    return "shadow longer" if fs > fm else "model longer"


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("a")
    ap.add_argument("b")
    ap.add_argument("--md")
    a = ap.parse_args()
    A, B = Path(a.a), Path(a.b)
    ra = {r["name"]: r for r in json.loads((A / "results.json").read_text(encoding="utf-8"))}
    rb = {r["name"]: r for r in json.loads((B / "results.json").read_text(encoding="utf-8"))}
    trig = trigger_windows()
    tot = {}
    model_same = model_diff = 0
    model_diff_names = []
    changed = []
    for name in ra:
        x, y = ra[name], rb.get(name, {})
        if "shadow" not in x or "shadow" not in y:
            tot[("not run", "A")] = tot.get(("not run", "A"), 0) + ("shadow" not in x)
            tot[("not run", "B")] = tot.get(("not run", "B"), 0) + ("shadow" not in y)
            continue
        g = "trigger" if name in trig else "plain"
        out = {}
        for tag, r in (("A", x), ("B", y)):
            fs, ws = first_fail(r["shadow"], r["gd_death"], r["to"])
            fm, wm = first_fail(r["model"], r["gd_death"], r["to"])
            v = verdict(fs, fm)
            tot[(g, tag, v)] = tot.get((g, tag, v), 0) + 1
            out[tag] = (fs, ws, fm, wm, v)
        ta, tb = A / name / "model.trace.csv", B / name / "model.trace.csv"
        if ta.exists() and tb.exists() and ta.read_bytes() == tb.read_bytes() and out["A"][2:4] == out["B"][2:4]:
            model_same += 1
        else:
            model_diff += 1
            model_diff_names.append(name)
        if out["A"][:2] != out["B"][:2] or out["A"][4] != out["B"][4]:
            changed.append((name, g, out, y))
    L = []
    L.append("| group | windows | run | shadow exact longer | model exact longer | both exact | both fail at the same tick |")
    L.append("|---|---|---|---|---|---|---|")
    for g in ("plain", "trigger"):
        for tag, lab in (("A", "A: " + A.name), ("B", "B: " + B.name)):
            sub = {k[2]: n for k, n in tot.items() if len(k) == 3 and k[0] == g and k[1] == tag}
            L.append(f"| {g} | {sum(sub.values())} | {lab} | " + " | ".join(str(sub.get(k, 0)) for k in
                     ("shadow longer", "model longer", "both exact", "same tick")) + " |")
    L.append(f"\nnot run: A {tot.get(('not run', 'A'), 0)}, B {tot.get(('not run', 'B'), 0)}")
    L.append(f"model arm: trace byte-equal and same first failure in {model_same} windows, different in {model_diff}"
             + (f" ({', '.join(model_diff_names)})" if model_diff_names else ""))
    L.append(f"\nwindows whose shadow result changed: {len(changed)}\n")
    L.append("| window | group | taken at | shadow A | shadow B | model | verdict A -> B | layer words GD changed after the take, up to B's shadow failure (tick: word old -> new) | later changes in the window |")
    L.append("|---|---|---|---|---|---|---|---|---|")
    fmt = lambda t, w: "exact" if t == INF else f"{t} ({w})"
    fv = lambda v: f"{v:.9g}" if isinstance(v, float) else str(v)
    for name, g, out, y in changed:
        tk = taken_at(B, name)
        _, ch = layer_changes(name, tk if tk is not None else y["seed_tick"], y["to"])
        fb = out["B"][0]
        upto = [c for c in ch if c[0] <= fb]
        later = [c for c in ch if c[0] > fb]
        def summ(cs, lim=12):
            s = "; ".join(f"{t}: {k} {fv(o)} -> {fv(n)}" for t, k, o, n in cs[:lim])
            return (s + f"; ... ({len(cs)} changes)") if len(cs) > lim else (s or "none")
        L.append(f"| {name} | {g} | {tk} | {fmt(*out['A'][:2])} | {fmt(*out['B'][:2])} | {fmt(*out['B'][2:4])} | "
                 f"{out['A'][4]} -> {out['B'][4]} | {summ(upto)} | {summ(later, 4)} |")
    txt = "\n".join(L) + "\n"
    print(txt)
    if a.md:
        Path(a.md).write_text(txt, encoding="utf-8")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
