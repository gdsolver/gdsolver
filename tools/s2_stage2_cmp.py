"""S2 stage 2: the model arm of two tools/s2_stage1.py runs side by side -- without --shadowstep (A) and with it (B) -- each against GD's dump.

    python tools/s2_stage2_cmp.py <run A dir> <run B dir> [--md <file>]

Per window: how many ticks the shadow stepped in B (its `shadowstep:` line), each arm's first failure (tools/s2_stage1_table.first_fail: |dx| or
|dy| > 0.01, a mode / gravity / size flag, a death that differs from GD's), max |dy|, the death tick, and the verdict: B better (fails later or
never), B worse (fails earlier), same. A window counts for the flag only if B stepped at least one tick there. The model arm's wall time is summed
per run (results.json model_seconds).
"""
from __future__ import annotations

import argparse
import json
import re
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from s2_stage1_table import INF, first_fail, trigger_windows  # noqa: E402


def stepped(r: dict) -> int:
    m = re.search(r"stepped (\d+)", r.get("shadowstep") or "")
    return int(m.group(1)) if m else 0


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("a")
    ap.add_argument("b")
    ap.add_argument("--md")
    a = ap.parse_args()
    ra = {r["name"]: r for r in json.loads((Path(a.a) / "results.json").read_text(encoding="utf-8"))}
    rb = {r["name"]: r for r in json.loads((Path(a.b) / "results.json").read_text(encoding="utf-8"))}
    trig = trigger_windows()
    rows = ["| window | group | stepped | A first fail | A max dy | A death | B first fail | B max dy | B death | GD death | verdict |", "|" + "---|" * 11]
    tot: dict = {}
    secs = [0.0, 0.0]
    for name in ra:
        x, y = ra[name], rb.get(name)
        if not y or "model" not in x or "model" not in y:
            continue
        secs[0] += x.get("model_seconds", 0.0)
        secs[1] += y.get("model_seconds", 0.0)
        n = stepped(y)
        to, gdd = x["to"], x["gd_death"]
        fa, wa = first_fail(x["model"], gdd, to)
        fb, wb = first_fail(y["model"], gdd, to)
        if n == 0:
            v = "not stepped" + ("" if (fa, wa) == (fb, wb) else " (DIFFERS)")
        elif fa == fb:
            v = "same"
        elif fb > fa:
            v = "B better"
        else:
            v = "B worse"
        g = "trigger" if name in trig else "plain"
        tot[(g, v)] = tot.get((g, v), 0) + 1
        f = lambda t, w: "exact" if t == INF else f"{t} ({w})"
        dd = lambda d: "-" if d is None or d < 0 else str(d)
        rows.append(f"| {name} | {g} | {n} | {f(fa, wa)} | {x['model']['max_dy']:.3g} | {dd(x['model'].get('died_in_window'))} | "
                    f"{f(fb, wb)} | {y['model']['max_dy']:.3g} | {dd(y['model'].get('died_in_window'))} | {dd(gdd)} | {v} |")
    lines = []
    for g in ("plain", "trigger"):
        sub = {k[1]: c for k, c in tot.items() if k[0] == g}
        lines.append(f"{g}: " + ", ".join(f"{k} {c}" for k, c in sorted(sub.items())))
    lines.append(f"model arm wall time: A {secs[0]:.1f} s, B {secs[1]:.1f} s")
    print("\n".join(lines))
    if a.md:
        Path(a.md).write_text("\n".join(lines) + "\n\n" + "\n".join(rows) + "\n", encoding="utf-8")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
