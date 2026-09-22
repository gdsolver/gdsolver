"""cold_regress's verdict and bless rules, checked on saved results -- no GD.

What is pinned (AUD-20260922-26/-28):
  * the iteration cap is printed, never a failure;
  * a bless needs a known resolution, and on a coin run every level at full coins;
  * --adopt blesses from a saved --one-session run and writes that run's commit,
    package and resolution into the baseline's _meta;
  * gdsave.resolution_index reads the profile's key, and run_resolution fails
    closed on a hand-configured profile that has none.

usage: python py/test_cold_regress_verdict.py   (exit 1 on the first failure)
"""
import contextlib
import io
import json
import sys
import tempfile
import types
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import cold_regress as C          # noqa: E402
from gdtas import gdsave          # noqa: E402

SECTION = """suite: level={lv} ({i}/{n})
dpsolve: iter 1: death t=100 x=130.0 (best t=-1)
dpsolve:   [fp] it=1 plan=10/aaaa t=100 x=130.000 fix=0/- live=-
{extra}dpsolve: solution saved -> solution_lv{lv}_dp.txt
{coins}level record changed: none (restored 3)
session_end: level_complete
"""


def suite(levels, coins=None, iters=None):
    out = []
    for i, lv in enumerate(levels, 1):
        extra = "".join(f"dpsolve: iter {k}: death t=1 x=1.0 (best t=-1)\n"
                        for k in range(2, (iters or {}).get(lv, 1) + 1))
        c = f"coingd: level complete with {coins[lv]} coins (pickup ticks: 1 2 3) unmatched=0\n" \
            if coins else ""
        out.append(SECTION.format(lv=lv, i=i, n=len(levels), extra=extra, coins=c))
    return "".join(out) + "suite: done\n"


def results_of(txt):
    res = []
    for lv, sec in C.split_suite(txt):
        r = C.read_result(sec)
        r["lv"], r["wall"] = lv, 1.0
        res.append(r)
    return res


def run(fn, *args):
    buf = io.StringIO()
    with contextlib.redirect_stdout(buf):
        rc = fn(*args)
    return rc, buf.getvalue()


def check(cond, what):
    if not cond:
        print("FAIL:", what)
        sys.exit(1)
    print("ok  ", what)


def main():
    tmp = Path(tempfile.mkdtemp(prefix="coldverdict_"))
    C.BASELINE, C.BASELINE_COINS = tmp / "off.json", tmp / "coins.json"
    ns = lambda **k: types.SimpleNamespace(**{"cfg": [], "levels": [1, 2], "bless": False,
                                              "mod": Path(__file__), "resolution": 25, **k})

    # the cap is not a verdict
    base = {"1": {"iters": 1, "fp": ""}, "2": {"iters": 1, "fp": ""}}
    res = results_of(suite([1, 2], iters={2: 40}))
    rc, out = run(C.report, res, base, ns())
    check(rc == 0 and "over the iteration cap (not a failure" in out,
          "40 iterations against a cap of 30 is printed and the run passes")

    # a bless needs a known resolution
    rc, out = run(C.report, results_of(suite([1, 2])), base, ns(bless=True, resolution=None))
    check(rc == 1 and not C.BASELINE.exists(), "a bless with an unknown resolution is refused")

    # ...and on a coin run, all coins on every level
    cfg = list(C.COIN_KEYS)
    rc, out = run(C.report, results_of(suite([1, 2], coins={1: "3/3", 2: "2/3"})), {},
                  ns(bless=True, cfg=cfg))
    check("NOT BLESSED" in out and not C.BASELINE_COINS.exists(),
          "a coin bless with 2/3 on a level is refused")

    # --adopt: from a saved one-session run, with that run's provenance
    d = tmp / "run"
    d.mkdir()
    (d / "coldlog_suite.txt").write_text(suite([1, 2], coins={1: "3/3", 2: "3/3"}, iters={2: 4}),
                                         encoding="utf-8")
    pkg = tmp / "pkg.geode"
    pkg.write_bytes(b"package")
    import hashlib
    meta = {"head": "abc1234def", "mod": str(pkg),
            "mod_sha256": hashlib.sha256(b"package").hexdigest(),
            "cfg": cfg, "levels": [1, 2], "resolution": 25, "arrangement": "one-session"}
    (d / "coldlog_suite.meta.json").write_text(json.dumps(meta), encoding="utf-8")
    rc, out = run(C.adopt, types.SimpleNamespace(adopt=d))
    got = json.loads(C.BASELINE_COINS.read_text()) if C.BASELINE_COINS.exists() else {}
    check(rc == 0 and got.get("2", {}).get("iters") == 4 and got.get("2", {}).get("coins") == "3/3"
          and got.get("_meta", {}).get("head") == "abc1234def"
          and got.get("_meta", {}).get("resolution") == 25,
          "--adopt writes the coin baseline with the run's commit and resolution")
    pkg.write_bytes(b"changed")
    rc, out = run(C.adopt, types.SimpleNamespace(adopt=d))
    check(rc == 2 and "no longer the package" in out, "--adopt refuses when the package changed")

    # gdsave: the key, and failing closed without it on a hand-configured profile
    prof = tmp / "prof"
    prof.mkdir()
    real = gdsave.save_paths
    gdsave.save_paths = lambda w: (prof / f"{w}.dat", prof / f"{w}b.dat")
    try:
        def write(w, xml):
            p = prof / f"{w}.dat"
            p.write_bytes(gdsave.encode_save(xml))
        xml25 = '<?xml version="1.0"?><plist><dict><k>resolution</k><i>25</i></dict></plist>'
        write(1, xml25)
        check(gdsave.resolution_index(1) == 25, "resolution_index reads the key")
        import os
        # incompressible, so the saved file is past the 1,000 B of a disposable profile
        big = '<?xml version="1.0"?><plist><dict><k>pad</k><s>' + os.urandom(3000).hex() + "</s></dict></plist>"
        write(2, big)
        check(gdsave.resolution_index(2) is None, "no key -> None")
        r, why = C.run_resolution([2])
        check((prof / "2.dat").stat().st_size > 1000 and r is None,
              "a large profile without the key stays unknown (fail closed)")
        r, why = C.run_resolution([1, 2])
        check(why != "", "workers that disagree (one of them unknown) are refused")
    finally:
        gdsave.save_paths = real
    print("all ok")


if __name__ == "__main__":
    main()
