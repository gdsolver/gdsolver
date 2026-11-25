#!/usr/bin/env python
"""Does a level solved in the SAME game, with its own cfg, start as a launch with that cfg would?

    python py/cfg_swap_check.py --worker 102 --mod <.geode> --out <dir> \
        --levels 16 17 18 --level-cfg cfgs.json

cfgs.json is {"17": ["trigtrace=1", ...], ...}: each level's cfg lines on top of cold_regress's own.

Two arms over the same levels:
  fresh    one launch per level (`cold_regress.py --levels L --cfg ...`)
  session  ONE game, every level with its own cfg (`--one-session --level-cfg`; the mod reads the
           level's whole cfg when it enters the level, cfg `levelcfgdir`)

and, per level, three things that have to be identical: the session's `cfgeff:` line (every cfg key
as the mod held it -- the place a value left behind by the previous level shows up; only the two keys
that exist to run the session, `levels` and `levelcfgdir`, are set aside), the last `[fp]` line
(the loop's fingerprint, which carries the iteration), and the verdict of the `level record changed:` line ("none"). The "(restored N)" count behind it
is one higher for every level after the first in a game -- GD's own inline attempt-counter write,
the same in a plain --one-session -- so it is printed and not compared.
Exit 1 on any difference.
"""

from __future__ import annotations

import argparse
import json
import re
import shutil
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO = HERE.parent
DATA = REPO / "data"
SESSION_ONLY = {"levels", "levelcfgdir"}


def run_cr(args: list[str]) -> int:
    cmd = [sys.executable, str(HERE / "cold_regress.py"), *args]
    print("  $", " ".join(cmd), flush=True)
    return subprocess.call(cmd, cwd=str(REPO))


def facts(log: Path) -> dict:
    txt = log.read_text(encoding="utf-8", errors="replace") if log.exists() else ""
    eff = re.findall(r"^cfgeff: (.*)$", txt, re.M)
    fps = re.findall(r"^dpsolve:   \[fp\] (.*)$", txt, re.M)
    rec = re.findall(r"^level record changed: (.*)$", txt, re.M)
    pairs = {}
    for tok in (eff[0].split(" ") if eff else []):
        k, _, v = tok.partition("=")
        if k not in SESSION_ONLY:
            pairs[k] = v
    return {"cfgeff": pairs, "fp": fps[-1] if fps else "", "record": rec[-1] if rec else "",
            "has_cfgeff": bool(eff)}


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--worker", type=int, required=True)
    ap.add_argument("--mod", type=Path, required=True)
    ap.add_argument("--levels", type=int, nargs="+", required=True)
    ap.add_argument("--level-cfg", type=Path, required=True)
    ap.add_argument("--out", type=Path, required=True)
    ap.add_argument("--arms", nargs="*", default=["fresh", "session"],
                    choices=["fresh", "session"])
    a = ap.parse_args(argv)
    cfgs = {int(k): v for k, v in json.loads(a.level_cfg.read_text(encoding="utf-8")).items()}
    a.out.mkdir(parents=True, exist_ok=True)
    common = ["--pool", str(a.worker), "--mod", str(a.mod)]
    # a log left in data/ by an earlier run must not stand in for one this run did not write
    for lv in a.levels:
        (DATA / f"coldlog_lv{lv}.txt").unlink(missing_ok=True)
    if "fresh" in a.arms:
        for lv in a.levels:
            run_cr(["--levels", str(lv), *common, "--cfg", *cfgs.get(lv, [])]
                   if cfgs.get(lv) else ["--levels", str(lv), *common])
            d = a.out / "fresh"
            d.mkdir(exist_ok=True)
            shutil.copyfile(DATA / f"coldlog_lv{lv}.txt", d / f"coldlog_lv{lv}.txt")
    if "session" in a.arms:
        for lv in a.levels:
            (DATA / f"coldlog_lv{lv}.txt").unlink(missing_ok=True)
        run_cr(["--one-session", "--levels", *map(str, a.levels), *common,
                "--level-cfg", str(a.level_cfg)])
        d = a.out / "session"
        d.mkdir(exist_ok=True)
        for lv in a.levels:
            shutil.copyfile(DATA / f"coldlog_lv{lv}.txt", d / f"coldlog_lv{lv}.txt")
    bad = 0
    print("== fresh vs session, per level")
    for lv in a.levels:
        f = facts(a.out / "fresh" / f"coldlog_lv{lv}.txt")
        s = facts(a.out / "session" / f"coldlog_lv{lv}.txt")
        diffs = [k for k in sorted(set(f["cfgeff"]) | set(s["cfgeff"]))
                 if f["cfgeff"].get(k) != s["cfgeff"].get(k)]
        ok = (f["has_cfgeff"] and s["has_cfgeff"] and not diffs and f["fp"] == s["fp"]
              and f["record"].split(" (restored")[0] == s["record"].split(" (restored")[0])
        bad += 0 if ok else 1
        print(f"  {'EQUAL ' if ok else 'DIFFER'} lv{lv}: cfgeff keys {len(f['cfgeff'])}/"
              f"{len(s['cfgeff'])} differ={len(diffs)}  fp {'=' if f['fp'] == s['fp'] else 'DIFF'}"
              f"  record {s['record']!r}")
        for k in diffs[:12]:
            print(f"      cfgeff {k}: fresh={f['cfgeff'].get(k)!r} session={s['cfgeff'].get(k)!r}")
        if f["fp"] != s["fp"]:
            print("      fp fresh  :", f["fp"][:140])
            print("      fp session:", s["fp"][:140])
    print(f"{len(a.levels) - bad}/{len(a.levels)} levels equal")
    return 0 if bad == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
