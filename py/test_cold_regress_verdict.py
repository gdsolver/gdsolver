"""cold_regress's verdict and bless rules, checked on saved results -- no GD.

What is pinned:
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

    # --fresh-verify: a level whose saved solution does not clear from the start is no longer CLEARED
    import verify_solutions as VS
    data = tmp / "fvdata"
    data.mkdir()
    for lv in (1, 2, 3, 5, 6):
        (data / f"solution_lv{lv}_dp.txt").write_text("input=10,1\n", encoding="utf-8")
    seen = []
    DIED = ["killer: t=27928 who=p2 py=345.0", "death: attempt=1 tick=27928 x=36197.984375 wallMs=1 speedX=41.0"]

    def fake_verify(w, path, lv, timeout_s, root, force_coins=False, extra_cfg=None, mod_file=None):
        seen.append((lv, mod_file))
        if lv == 5:
            raise RuntimeError("the worker would not start")
        if lv == 6:
            return VS.Verdict(path.name, lv, False, "", w, [], ["dpsolve: ..."], True)
        if lv == 2:
            return VS.Verdict(path.name, lv, False, "no complete", w, [], list(DIED), False)
        return VS.Verdict(path.name, lv, True, "", w, [], ["complete: ..."], False)

    real_vo = VS.verify_one
    VS.verify_one = fake_verify
    pkg2 = tmp / "pkg2.geode"
    pkg2.write_bytes(b"package2")
    try:
        fv = results_of(suite([1, 2, 3, 5, 6]))
        fv.append({"lv": 4, "cleared": False, "why": "stuck"})
        out_dir = tmp / "fvout"
        out_dir.mkdir()
        run(C.fresh_verify, fv, 99, pkg2, out_dir, None, data)
        fv_missing = results_of(suite([7]))              # cleared in the log, no solution file
        run(C.fresh_verify, fv_missing, 99, pkg2, out_dir, None, data)
    finally:
        VS.verify_one = real_vo
    by = {r["lv"]: r for r in fv}
    check([lv for lv, _ in seen] == [1, 2, 3, 5, 6] and all(m == pkg2 for _, m in seen),
          "fresh-verify replays every cleared level on the pinned package, and only those")
    check(by[1]["cleared"] and by[1]["fresh_verify"] == "ok" and by[3]["cleared"],
          "a level that clears from the start stays CLEARED")
    check(not by[2]["cleared"] and by[2]["why"] == "stuck (mid_session_only)"
          and by[2]["fresh_verify"] == "death" and "t=27928" in by[2]["fresh_verify_detail"]
          and "p2" in by[2]["fresh_verify_detail"],
          "a replay that dies is stuck (mid_session_only), with the tick and who died")
    check(by[5]["why"].startswith("unverified") and by[6]["why"].startswith("unverified")
          and by[5]["fresh_verify"] == "instrument" and by[6]["fresh_verify"] == "instrument"
          and fv_missing[0]["why"].startswith("unverified") and not fv_missing[0]["cleared"],
          "an exception, a timeout and a missing plan are `unverified`, not a death")
    check(by[4]["why"] == "stuck" and "fresh_verify" not in by[4], "a stuck level is not replayed")
    # a launch ended for a silent log says where it stopped and under what load; the verdict stays `instrument` (unverified), as for any clock
    st = {"quiet_s": 90, "last_line": "dpconfig: package=1/abcd", "gd_running": 4, "mem_free_gb": 7.5}
    k_stall = C.fresh_classify(["session_start"], False, "no complete", True, st)
    k_plain = C.fresh_classify(["session_start"], False, "no complete", True)
    check(k_stall[0] == "instrument" and k_stall[1].startswith("stalled: result.txt quiet for 90s after \"dpconfig: package=1/abcd\"")
          and "4 game process(es)" in k_stall[1] and "7.5 GB free" in k_stall[1] and k_stall[2] is None,
          "a stalled launch is an `instrument` verdict that names the last line, the quiet time, the game count and the free memory")
    check(k_plain[0] == "instrument" and k_plain[1].startswith("timeout (") and C.fresh_classify([], True, "x", True, st)[0] == "ok",
          "without a stall the timeout text is as it was, and a clear is a clear whatever the stall says")
    check((out_dir / "solution_lv2_dp.txt").exists(), "the judged solution is kept beside the logs")
    side2 = json.loads((out_dir / "fresh_verify_lv2.json").read_text(encoding="utf-8"))
    check(side2["requested"] and side2["kind"] == "death" and side2["death"]["tick"] == 27928
          and side2["mod_sha256"] == C._sha256(pkg2) and len(side2["plan_sha256"]) == 64
          and (out_dir / side2["log"]).read_text(encoding="utf-8").startswith("killer:"),
          "the sidecar keeps the request, both SHA256s, the death and the replay's raw log")

    # --fresh-verify-one-session: every cleared level's saved solution in one game, the verdicts read from each level's own section
    data1 = tmp / "fv1data"
    data1.mkdir()
    for lv in (1, 2, 3):
        (data1 / f"solution_lv{lv}_dp.txt").write_text("input=10,1\n", encoding="utf-8")
    OKSEC = "suite: level={lv} ({i}/3)\nsuiteplans: level={lv} plan=x inputs=1\ncomplete: attempt=1 step=0 tick=100 x=130 levelMaxX=100 goalX=100 pct=100.000000\nsession_end: level_complete\n"
    DIESEC = ("suite: level=2 (2/3)\nsuiteplans: level=2 plan=x inputs=1\nkiller: t=50 who=p1 py=1.0\n"
              "death: attempt=1 tick=50 x=77.5 wallMs=1 speedX=1.0\nsession_end: max_attempts\n")
    stall = {"quiet_s": 90, "last_line": "dpconfig: package=1/abcd", "gd_running": 4, "mem_free_gb": 7.5}
    cfgs = []

    def fake_run_session(wid, cfg, **kw):
        cfgs.append((list(cfg), kw))
        txt = OKSEC.format(lv=1, i=1) + DIESEC + "suite: level=3 (3/3)\n"        # level 3 is entered and then the log goes quiet
        return types.SimpleNamespace(lines=txt.split("\n"), timed_out=True, crash="", stall=stall)

    retried = []

    def fake_verify_retry(w, path, lv, timeout_s, root, force_coins=False, extra_cfg=None, mod_file=None):
        retried.append(lv)
        return VS.Verdict(path.name, lv, True, "complete: ...", w, [], ["complete: ..."], False)

    real_rs, real_vo = C.run_session, VS.verify_one
    C.run_session, VS.verify_one = fake_run_session, fake_verify_retry
    try:
        r1 = results_of(suite([1, 2, 3]))
        out1 = tmp / "fv1out"
        out1.mkdir()
        _, printed = run(C.fresh_verify_one_session, r1, 99, pkg2, out1, None, data1)
    finally:
        C.run_session, VS.verify_one = real_rs, real_vo
    cfg1, kw1 = cfgs[0]
    by1 = {r["lv"]: r for r in r1}
    check("levels=1,2,3" in cfg1 and any(c.startswith("suiteplans=") and c.endswith("fresh_plans") for c in cfg1) and "solve=0" in cfg1
          and kw1.get("done_marker") == C.SUITE_DONE and (out1 / "fresh_plans" / "solution_lv2_dp.txt").exists(),
          "the one-session replay is one bare game over the cleared levels, with their plans in a directory the mod reads")
    check(by1[1]["fresh_verify"] == "ok" and by1[2]["fresh_verify"] == "death" and "t=50" in by1[2]["fresh_verify_detail"]
          and by1[2]["why"] == "stuck (mid_session_only)",
          "each level's verdict is read from its own section: a clear is ok, a death is stuck (mid_session_only) with its tick")
    check(retried == [3] and by1[3]["fresh_verify"] == "ok" and by1[3]["cleared"],
          "the level the game stalled in is played once more by itself, and only that one")
    s3 = json.loads((out1 / "fresh_verify_lv3.json").read_text(encoding="utf-8"))
    ss = json.loads((out1 / "fresh_verify_suite.json").read_text(encoding="utf-8"))
    check(s3["per_launch_retry"] and s3["one_session_first"]["kind"] == "instrument" and s3["one_session_first"]["stall"]["quiet_s"] == 90
          and ss["stalled"] and ss["retried_per_launch"] == [3] and ss["kinds_first_attempt"]["3"] == "instrument"
          and "1 stall(s)" in printed and "per-launch retry for [3]" in printed,
          "the retry is recorded in the sidecar (what the first attempt said, with its stall) and in the suite's summary and output")
    side1 = json.loads((out1 / "fresh_verify_lv1.json").read_text(encoding="utf-8"))
    check(side1["arrangement"] == "one-session" and side1["kind"] == "ok" and len(side1["plan_sha256"]) == 64 and side1["mod_sha256"] == C._sha256(pkg2),
          "a level's sidecar has the same fields as the per-launch form, and says it was one-session")
    # without the retry the stalled level stays unverified and says where the game stopped
    C.run_session, VS.verify_one = fake_run_session, fake_verify_retry
    try:
        r2 = results_of(suite([1, 2, 3]))
        out2 = tmp / "fv2out"
        out2.mkdir()
        run(C.fresh_verify_one_session, r2, 99, pkg2, out2, None, data1, False)
    finally:
        C.run_session, VS.verify_one = real_rs, real_vo
    by2 = {r["lv"]: r for r in r2}
    check(by2[3]["why"].startswith("unverified") and not by2[3]["cleared"]
          and json.loads((out2 / "fresh_verify_lv3.json").read_text(encoding="utf-8"))["kind"] == "instrument",
          "with the retry off a stalled level stays unverified")
    # a replay that ends in a death leaves the suite unable to leave the level: the game says so and stops, the levels after it are played again by themselves
    def fake_run_session_dead(wid, cfg, **kw):
        txt = (OKSEC.format(lv=1, i=1) + DIESEC.replace("(2/3)", "(2/3)") + "suite: gave up waiting to leave the level after 60s (playLayer=still up) - the remaining levels are not run\n"
               "suite: done 3 levels\n")
        return types.SimpleNamespace(lines=txt.split("\n"), timed_out=False, crash="", stall=None)

    retried.clear()
    C.run_session, VS.verify_one = fake_run_session_dead, fake_verify_retry
    try:
        r3 = results_of(suite([1, 2, 3]))
        out3 = tmp / "fv3out"
        out3.mkdir()
        run(C.fresh_verify_one_session, r3, 99, pkg2, out3, None, data1)
    finally:
        C.run_session, VS.verify_one = real_rs, real_vo
    ss3 = json.loads((out3 / "fresh_verify_suite.json").read_text(encoding="utf-8"))
    s33 = json.loads((out3 / "fresh_verify_lv3.json").read_text(encoding="utf-8"))
    check(retried == [3] and not ss3["stalled"] and ss3["stopped_after_a_level_it_could_not_leave"]
          and "could not leave the level" in s33["one_session_first"]["detail"] and {r["lv"]: r for r in r3}[2]["fresh_verify"] == "death",
          "a death that stops the suite is not counted as a stall, says why the later levels were not reached, and they are played again by themselves")
    # the bare cfg of the one-session replay is the per-launch one: verify_one's list without its level and plan lines
    seen_cfg = []
    VS.run_session = lambda w, cfg, **kw: (seen_cfg.append(list(cfg)), types.SimpleNamespace(lines=["complete: x"], timed_out=False))[1]
    try:
        VS.verify_one(99, data1 / "solution_lv1_dp.txt", 5, 10.0, Path("."), False, None, pkg2)
    finally:
        VS.run_session = real_rs
    check([c for c in seen_cfg[0] if not c.startswith("level=") and not c.startswith("input=")] == C.FRESH_BARE_CFG,
          "the one-session replay's bare cfg is the per-launch replay's")

    # ...and --adopt reads the sidecars: the verdict does not go back to CLEARED
    import hashlib
    ad = tmp / "advf"
    ad.mkdir()
    (ad / "coldlog_suite.txt").write_text(suite([1, 2], coins={1: "3/3", 2: "3/3"}), encoding="utf-8")
    pkg3 = tmp / "pkg3.geode"
    pkg3.write_bytes(b"package3")
    sha3 = hashlib.sha256(b"package3").hexdigest()
    meta3 = {"head": "abc1234def", "mod": str(pkg3), "mod_sha256": sha3, "cfg": list(C.COIN_KEYS),
             "levels": [1, 2], "resolution": 25, "arrangement": "one-session", "fresh_verify": True}
    (ad / "coldlog_suite.meta.json").write_text(json.dumps(meta3), encoding="utf-8")
    rc, out = run(C.adopt, types.SimpleNamespace(adopt=ad))
    check(rc == 2 and "unknown" in out, "--adopt of a fresh-verify run without its sidecars is refused")
    for lv, kind in ((1, "ok"), (2, "death")):
        (ad / f"fresh_verify_lv{lv}.json").write_text(json.dumps(
            {"requested": True, "level": lv, "mod_sha256": sha3, "kind": kind,
             "detail": "" if kind == "ok" else "died at t=27928 x=36198.0 (p2)"}), encoding="utf-8")
    C.BASELINE_COINS.unlink(missing_ok=True)
    rc, out = run(C.adopt, types.SimpleNamespace(adopt=ad))
    check(rc != 0 and "mid_session_only" in out and not C.BASELINE_COINS.exists(),
          "--adopt reads the sidecar: lv2 stays stuck (mid_session_only) and nothing is blessed")
    (ad / "fresh_verify_lv2.json").write_text(json.dumps(
        {"requested": True, "level": 2, "mod_sha256": "0" * 64, "kind": "ok"}), encoding="utf-8")
    rc, out = run(C.adopt, types.SimpleNamespace(adopt=ad))
    check(rc == 2 and "not a fresh-verify verdict" in out, "--adopt refuses a sidecar of another package")
    # a level the runner's guard ended says so in its `why` (the memory floor read as "out of wall time")
    g1 = {"lv": 1, "cleared": False, "why": "stuck: out of wall time", "stopped": "memory floor: 3051 MB available, under 3072 MB"}
    C._say_guard_stop(g1)
    check(g1["why"].startswith("stuck: stopped by the guard (memory floor: 3051 MB") and g1["stopped"].startswith("memory floor"),
          "a level stopped by the memory floor says so in its summary `why`")
    g2 = {"lv": 2, "cleared": True, "why": "CLEARED", "stopped": "level deadline 1200s"}
    g3 = {"lv": 3, "cleared": False, "why": "crash: boom", "stopped": "memory floor: 1 MB"}
    g4 = {"lv": 4, "cleared": False, "why": "stuck: out of wall time"}
    for g in (g2, g3, g4):
        C._say_guard_stop(g)
    check(g2["why"] == "CLEARED" and g3["why"] == "crash: boom" and g4["why"] == "stuck: out of wall time",
          "a cleared level, a crash and a level the guard did not stop keep their `why`")
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
