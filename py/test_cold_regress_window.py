"""cold_regress's comparison of measuring conditions, on windows -- no GD.

What is pinned:
  * a resolution index is a position in the display's mode list (another size on another machine), so when both the baseline and the run know their
    window the comparison is on sizes: the same side of the saw-radius threshold (heights 900 and above / 864 and below) is comparable;
  * a height between the two (not measured) is comparable only with the very same height;
  * a baseline whose _meta has no window is compared by index, unless --baseline-window says the size;
  * gdsave.window_size reads the machine's own map (<workers root>/resolution_sizes.json) before the lab table, and answers None for an unknown index;
  * the window goes into a blessed baseline's _meta and the run's meta (--adopt).

usage: python py/test_cold_regress_window.py   (exit 1 on the first failure)
"""
import json
import sys
import tempfile
import types
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import cold_regress as C          # noqa: E402
from gdtas import gdsave          # noqa: E402


def check(name, cond):
    print(("ok   " if cond else "FAIL ") + name)
    if not cond:
        sys.exit(1)


def run(window, resolution, baseline_window=None):
    return types.SimpleNamespace(window=window, resolution=resolution, baseline_window=baseline_window)


NEW = {"_meta": {"resolution": 25, "window": [1706, 960]}}
OLD = {"_meta": {"resolution": 25}}

# sizes
check("1706x960 and 1600x900 are on the same side", gdsave.same_saw_side((1706, 960), (1600, 900)))
check("1280x720 and 1706x960 are not", not gdsave.same_saw_side((1280, 720), (1706, 960)))
check("864 and 720 are both small", gdsave.same_saw_side((1536, 864), (1280, 720)))
check("a height between 864 and 900 matches only itself", gdsave.same_saw_side((1500, 880), (1600, 880)) and not gdsave.same_saw_side((1500, 880), (1706, 960)))
check("_wh parses WxH", C._wh("1600x900") == (1600, 900) and C._wh(" 1600 X 900 ") == (1600, 900))

# the verdict
ref, line = C.baseline_resolution_check(NEW, run((1600, 900), 16))
check("a baseline with a window: another index on the same side is compared, and says so", not ref and "1600x900" in line and "1706x960" in line)
ref, line = C.baseline_resolution_check(NEW, run((1600, 900), 25))
check("...also the same index (a machine whose index 25 is 1600x900)", not ref)
ref, line = C.baseline_resolution_check(NEW, run((1706, 960), 25))
check("the same window prints nothing", not ref and line == "")
ref, line = C.baseline_resolution_check(NEW, run((1280, 720), 21))
check("the other side of the threshold is refused with both sizes", ref and "1706x960" in line and "1280x720" in line)
ref, line = C.baseline_resolution_check(OLD, run((1600, 900), 16))
check("an old baseline is compared by index: refused, with the hint to give its window", ref and "--baseline-window" in line)
ref, line = C.baseline_resolution_check(OLD, run((1600, 900), 16, baseline_window=(1706, 960)))
check("...unless --baseline-window says its size", not ref)
ref, line = C.baseline_resolution_check(OLD, run((1706, 960), 25))
check("an old baseline at the same index is compared as before", not ref and line == "")
ref, line = C.baseline_resolution_check(NEW, run(None, 16))
check("a run whose window is unknown falls back to the index (refused, with the --window hint)", ref and "--window" in line)
ref, line = C.baseline_resolution_check(NEW, run(None, 25))
check("...and passes at the same index", not ref)
ref, line = C.baseline_resolution_check({}, run((1600, 900), 16))
check("no baseline: nothing to compare", not ref and line == "")
ref, line = C.baseline_resolution_check({"_meta": {}}, run((1600, 900), 16))
check("a baseline without a resolution is noted, not refused", not ref and "does not record" in line)

# window_size
with tempfile.TemporaryDirectory() as d:
    root = Path(d)
    check("an unknown index is None", gdsave.window_size(16, root) is None)
    check("the lab table answers 25", gdsave.window_size(25, root) == (1706, 960))
    (root / "resolution_sizes.json").write_text(json.dumps({"16": [1600, 900], "25": [1536, 864]}), encoding="utf-8")
    check("the machine's map answers 16", gdsave.window_size(16, root) == (1600, 900))
    check("the machine's map wins over the lab table", gdsave.window_size(25, root) == (1536, 864))
    check("an index the map lacks falls back to the lab table", gdsave.window_size(21, root) == (1280, 720))

# the window goes into _meta
a = types.SimpleNamespace(levels=[1, 2], window=(1600, 900), run_meta={"head": "abc", "mod_sha256": "d" * 64})
check("_meta carries the window", C._meta(a, False, 16)["window"] == [1600, 900])
a.window = None
check("_meta says null when the size is unknown", C._meta(a, False, 16)["window"] is None)
print("all ok")
