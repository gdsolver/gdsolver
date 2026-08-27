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


def set_resolution_index(worker_id: int | str, index: int) -> None:
    write_xml(worker_id, _set_int_key(read_xml(worker_id), "resolution", index))


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
