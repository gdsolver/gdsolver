"""The per-window table and the totals of an s2_stage1.py run.

    python tools/s2_stage1_table.py <out dir of s2_stage1.py>   -> table.md / totals.txt in that dir, and stdout

An arm's first failure at a tick is the earliest of: |dx| or |dy| > 0.01 against GD's dump, a mode / gravity / size
flag that differs, and a death that differs from GD's (a death GD did not have, at that tick; GD's death the arm
did not have, at GD's tick). An arm with none in the window is exact over it. Windows whose GD trigger writes are
known to matter (the 10-30 differs table: an Options / Gravity / Teleport timeline closes them or is named as
needed, or a note names such a trigger) are counted apart: neither arm is given those writes here.
"""
from __future__ import annotations

import json
import re
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "py"))
from gdtas.paths import LAB  # noqa: E402

DIFFERS_TSV = LAB / "notes" / "selfshadow-differs-table-1030.tsv"
TRIG_RE = re.compile(r"timeline|Options|Gravity|Teleport|teleport trigger|trigger-write")
INF = 1 << 60


def trigger_windows() -> dict[str, str]:
    out = {}
    for ln in DIFFERS_TSV.read_text(encoding="utf-8").splitlines()[1:]:
        c = ln.split("\t")
        if len(c) >= 6 and TRIG_RE.search(" ".join(c[4:])):
            m = TRIG_RE.search(" ".join(c[4:]))
            out[c[0]] = m.group(0)
    return out


def first_fail(arm: dict, gd_death: int, to: int) -> tuple[int, str]:
    cands = []
    if arm.get("first_off"):
        cands.append((arm["first_off"][0], f"{arm['first_off'][1]} {arm['first_off'][2]:+.4g}/{arm['first_off'][3]:+.4g}"))
    if arm.get("first_mode"):
        cands.append((arm["first_mode"][0], f"mode {arm['first_mode'][1]}!={arm['first_mode'][2]}"))
    if arm.get("first_up") is not None:
        cands.append((arm["first_up"], "gravity"))
    if arm.get("first_mini") is not None:
        cands.append((arm["first_mini"], "size"))
    d = arm.get("died_in_window", -1)
    gd_in = gd_death if 0 <= gd_death <= to else -1
    if d != gd_in:
        if d >= 0 and (gd_in < 0 or d < gd_in):
            cands.append((d, "false death"))
        elif gd_in >= 0:
            cands.append((gd_in, "no death" if d < 0 else f"death late ({d})"))
    if not cands:
        return INF, ""
    return min(cands)


def main() -> int:
    d = Path(sys.argv[1])
    res = json.loads((d / "results.json").read_text(encoding="utf-8"))
    trig = trigger_windows()
    rows, tot = [], {}
    hdr = ("| window | set | seed F | to | GD death | shadow first fail | shadow max dy / dvy | shadow death | "
           "model first fail | model max dy / dvy | model death | first > 0.001 (sh / model) | verdict |")
    sep = "|" + "---|" * 13
    for r in res:
        name = r["name"]
        if "shadow" not in r or "model" not in r:
            rows.append(f"| {name} | {r.get('set')} | - | {r.get('to')} | - | {r.get('error', '?')} | | | | | | | not run |")
            tot["not run"] = tot.get("not run", 0) + 1
            continue
        to, gdd = r["to"], r["gd_death"]
        s, m = r["shadow"], r["model"]
        fs, ws = first_fail(s, gdd, to)
        fm, wm = first_fail(m, gdd, to)
        if fs == INF and fm == INF:
            v = "both exact"
        elif fs == fm:
            v = "same tick"
        elif fs > fm:
            v = "shadow longer"
        else:
            v = "model longer"
        group = "trigger" if name in trig else "plain"
        tot[(group, v)] = tot.get((group, v), 0) + 1
        def f(t, w):
            return "exact" if t == INF else f"{t} ({w})"
        def dd(x):
            return "-" if x < 0 else str(x)
        fa = lambda a: "-" if not a.get("first_any") else str(a["first_any"][0])
        rows.append(f"| {name} | {r['set']}{' / trig: ' + trig[name] if name in trig else ''} | {r['seed_tick']} | {to} | {dd(gdd)} | "
                    f"{f(fs, ws)} | {s['max_dy']:.3g} / {s['max_dvy']:.3g} | {dd(s['died_in_window'])} | "
                    f"{f(fm, wm)} | {m['max_dy']:.3g} / {m['max_dvy']:.3g} | {dd(m['died_in_window'])} | "
                    f"{fa(s)} / {fa(m)} | {v} |")
    lines = [hdr, sep] + rows
    (d / "table.md").write_text("\n".join(lines) + "\n", encoding="utf-8")
    tl = []
    for g in ("plain", "trigger"):
        sub = {k[1]: n for k, n in tot.items() if isinstance(k, tuple) and k[0] == g}
        tl.append(f"{g}: {sum(sub.values())} windows: " + ", ".join(f"{k} {sub.get(k, 0)}" for k in
                  ("shadow longer", "model longer", "both exact", "same tick")))
    tl.append(f"not run: {tot.get('not run', 0)}")
    (d / "totals.txt").write_text("\n".join(tl) + "\n", encoding="utf-8")
    print("\n".join(tl))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
