"""Reading and writing the GD save (CCGameManager.dat).

The format is "XOR 11 -> URL-safe base64 -> gzip -> plist XML". Each worker has its
own root, and THE WINDOW MODE AND THE RESOLUTION ARE PERSISTED HERE TOO. GD restores
the saved resolution even if it is SetWindowPos'd from outside, so the only way to
set the window size is to write it here before the process starts.

Resolution indices (measured on this machine, client area):
    7=784x442   13=906x512   15=960x600   19=1120x700  20=1178x661
    21=1280x720 (16:9)  22=1280x800  23=1280x960  24=1365x1024
    25=1706x960 (16:9)
A brand-new save with no index comes up at 1202x902 = 4:3.

The index is a position in the list of display modes GD builds from the display, so the same index is another size on another machine: what a
run measured is a SIZE (GD's saw radius follows the window height), and window_size() gives the size an index means HERE.
"""

from __future__ import annotations

import base64
import gzip
import re
from pathlib import Path

from .paths import gd_save_root

# Minimal GD save (windowed / non-borderless). No account, no levels, no achievements.
# gv_0025 = windowed, gv_0170 = borderless.
# ALWAYS PUT resolution IN. Without it GD comes up at the default ~426x320 (4:3), and
# CHANGING THE SCREEN WIDTH CHANGES THE FIRING TICK OF CAMERA-DRIVEN TRIGGERS. On
# 2026-08-07 this contaminated a whole night of measurements: the verified lv19
# solution was reproducibly DEAD on worker-96 (no resolution) only, and reproducibly
# clear on 94/95/97 (16:9). Same plan, same exe, same inputs - the only difference was
# the worker's resolution.
# 21 (1280x720) and 25 (1706x960) are both 16:9, so they agree. What matters is the
# aspect ratio, not the pixel count.
#
# This template is used every time repair_save regenerates a save, so if it is not in
# here THE WORKER SILENTLY FALLS BACK TO 4:3. That alone makes verification of levels
# with moving geometry / triggers (lv19 onwards and lv20-22) untrustworthy.
MINIMAL_SAVE_XML = (
    '<?xml version="1.0"?><plist version="1.0" gjver="2.0"><dict>'
    '<k>resolution</k><i>25</i>'
    '<k>valueKeeper</k><d><k>gv_0025</k><s>1</s><k>gv_0170</k><s>0</s></d>'
    '</dict></plist>'
)


def decode_save(data: bytes) -> bytes:
    raw = bytes(b ^ 11 for b in data)
    s = raw.decode("ascii", "ignore").strip("\x00").replace("-", "+").replace("_", "/")
    return base64.b64decode(s + "=" * (-len(s) % 4))


def encode_save(xml: str) -> bytes:
    comp = gzip.compress(xml.encode("utf-8"))
    b64 = base64.b64encode(comp).decode("ascii").replace("+", "-").replace("/", "_")
    return bytes(b ^ 11 for b in (b64 + "\x00").encode("ascii"))


def save_paths(worker_id: int | str) -> tuple[Path, Path]:
    """(CCGameManager.dat, CCGameManager2.dat). GD reads both, so always keep them in sync."""
    root = gd_save_root(int(worker_id))
    return root / "CCGameManager.dat", root / "CCGameManager2.dat"


def read_xml(worker_id: int | str) -> str:
    primary, _ = save_paths(worker_id)
    if not primary.exists():
        return MINIMAL_SAVE_XML
    return gzip.decompress(decode_save(primary.read_bytes())).decode("utf-8")


def write_xml(worker_id: int | str, xml: str) -> None:
    primary, secondary = save_paths(worker_id)
    primary.parent.mkdir(parents=True, exist_ok=True)
    blob = encode_save(xml)
    primary.write_bytes(blob)
    secondary.write_bytes(blob)


def _set_int_key(xml: str, key: str, value: int) -> str:
    pat = re.compile(rf"(<k>{re.escape(key)}</k><i>)[^<]*(</i>)")
    if pat.search(xml):
        return pat.sub(rf"\g<1>{value}\g<2>", xml)
    # Insert into the root dict only (the first one)
    return xml.replace("<dict>", f"<dict><k>{key}</k><i>{value}</i>", 1)


def _set_str_key(xml: str, key: str, value: str) -> str:
    pat = re.compile(rf"(<k>{re.escape(key)}</k><s>)[^<]*(</s>)")
    if pat.search(xml):
        return pat.sub(rf"\g<1>{value}\g<2>", xml)
    return xml.replace("<k>valueKeeper</k><d>",
                       f"<k>valueKeeper</k><d><k>{key}</k><s>{value}</s>", 1)


# Sizes in GD's own units (client area as GD sees it, DPI-scaled) of the indices measured on the lab machine (the table in the docstring above).
LAB_SIZES = {7: (784, 442), 13: (906, 512), 15: (960, 600), 19: (1120, 700), 20: (1178, 661), 21: (1280, 720), 22: (1280, 800), 23: (1280, 960),
             24: (1365, 1024), 25: (1706, 960)}
# GD's saw radius follows the window height ([M] 10-04: the same at every height of 900 and above, another one at 864 and below; between the two
# it was not measured, so a window there is comparable only with the very same height).
SAW_BIG_FROM, SAW_SMALL_TO = 900, 864


def window_size(index: int, workers_root: Path | None = None) -> tuple[int, int] | None:
    """The window (width, height) the resolution index means on THIS machine: the machine's own map (<workers root>/resolution_sizes.json,
    {"16": [1600, 900]}, written by the calibration of gd06_setup_v2) when it names the index, else the lab machine's table. None when
    neither knows the index (then only the index itself can be compared)."""
    import json
    from .paths import WORKERS_ROOT
    p = Path(workers_root or WORKERS_ROOT) / "resolution_sizes.json"
    try:
        m = json.loads(p.read_text(encoding="utf-8-sig"))
        if str(index) in m:
            w, h = m[str(index)]
            return int(w), int(h)
    except (OSError, ValueError, TypeError):
        pass
    return LAB_SIZES.get(int(index))


def saw_side(height: int) -> str | None:
    """'big' (height >= 900), 'small' (<= 864), None between the two (not measured)."""
    return "big" if height >= SAW_BIG_FROM else ("small" if height <= SAW_SMALL_TO else None)


def same_saw_side(a: tuple[int, int], b: tuple[int, int]) -> bool:
    """Whether two windows give the same saw radius: the same side of the 864 / 900 threshold, or the very same height between the two."""
    sa, sb = saw_side(a[1]), saw_side(b[1])
    return (sa is not None and sa == sb) or a[1] == b[1]


def set_resolution_index(worker_id: int | str, index: int) -> None:
    write_xml(worker_id, _set_int_key(read_xml(worker_id), "resolution", index))


def resolution_index(worker_id: int | str) -> int | None:
    """The profile's `resolution` key, or None when it has none.

    It is part of what a run measured, not only of how the window looks: GD's
    saw radius follows it (lv20's id 187 dumps 21.87 at index 25 and 21.96 at
    index 8), and that alone changes the first plan of a cold."""
    try:
        m = re.search(r"<k>resolution</k><i>(-?\d+)</i>", read_xml(worker_id))
    except (OSError, ValueError):
        return None
    return int(m.group(1)) if m else None


def has_resolution(worker_id: int | str) -> bool:
    """Whether the resolution key exists. Without it GD comes up at 4:3 (MINIMAL_SAVE_XML)."""
    try:
        return "<k>resolution</k>" in read_xml(worker_id)
    except (OSError, ValueError):
        return False


def ensure_windowed(worker_id: int | str) -> None:
    """Guarantee windowed / non-borderless.

    Fullscreen eats +450MiB of private bytes plus GPU memory, and can bring a run to
    a halt. This is not decoration, it is required.
    """
    xml = read_xml(worker_id)
    out = _set_str_key(_set_str_key(xml, "gv_0025", "1"), "gv_0170", "0")
    write_xml(worker_id, out)
