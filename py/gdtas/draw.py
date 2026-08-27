"""Small drawing helpers (pillow)."""

from __future__ import annotations

import os
from pathlib import Path

from PIL import ImageFont

_FONT_DIR = Path(os.environ.get("SystemRoot", r"C:\Windows")) / "Fonts"


def mono_font(size: int, bold: bool = False):
    """Monospace font. Falls back to pillow's built-in one (where size has no effect)."""
    for name in (("consolab.ttf", "consola.ttf") if bold
                 else ("consola.ttf", "cour.ttf")):
        p = _FONT_DIR / name
        if p.exists():
            return ImageFont.truetype(str(p), size)
    return ImageFont.load_default()
