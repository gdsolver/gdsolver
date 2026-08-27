"""Lay several wall shots out in a labelled grid.

    python py/contact_sheet.py --images a.png b.png --out sheet.png --columns 2

Why it is needed: one frozen frame shows no **movement**. Which lane the player
is in, whether it is rising or falling, which gap it is aiming at -- none of that
reads until consecutive x's sit side by side. wall_shot.py produces one image per
x; this is what joins them up.
"""

from __future__ import annotations

import argparse
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

from PIL import Image, ImageDraw

from gdtas.draw import mono_font

PAD = 4
LABEL_H = 22
BG = (24, 24, 28)


def build(images: list[Path], out: Path, columns: int = 2, cell_width: int = 0,
          labels: list[str] | None = None) -> tuple[int, int, int]:
    bmps, names = [], []
    for p in images:
        if not Path(p).exists():
            print(f"missing: {p}")
            continue
        bmps.append(Image.open(p).convert("RGB"))
        names.append(Path(p).stem)
    if not bmps:
        raise SystemExit("no images")

    # Fit every cell to the size of the first one, or the grid falls apart
    cw = cell_width if cell_width > 0 else bmps[0].width
    ch = int(bmps[0].height * (cw / bmps[0].width))
    cols = min(columns, len(bmps))
    rows = -(-len(bmps) // cols)
    W = cols * cw + (cols + 1) * PAD
    H = rows * (ch + LABEL_H) + (rows + 1) * PAD

    sheet = Image.new("RGB", (W, H), BG)
    d = ImageDraw.Draw(sheet)
    font = mono_font(14, bold=True)
    labels = labels or []
    for i, bmp in enumerate(bmps):
        r, c = divmod(i, cols)
        x = PAD + c * (cw + PAD)
        y = PAD + r * (ch + LABEL_H + PAD)
        d.text((x, y), labels[i] if i < len(labels) else names[i],
               fill=(255, 255, 255), font=font)
        sheet.paste(bmp.resize((cw, ch), Image.LANCZOS), (x, y + LABEL_H))
        bmp.close()
    out.parent.mkdir(parents=True, exist_ok=True)
    sheet.save(out, "PNG")
    return W, H, len(bmps)


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--images", nargs="+", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--columns", type=int, default=2)
    ap.add_argument("--cell-width", type=int, default=0,
                    help="0 = keep the original width")
    ap.add_argument("--labels", nargs="*", default=[])
    a = ap.parse_args(argv)

    W, H, n = build([Path(p) for p in a.images], Path(a.out), a.columns,
                    a.cell_width, a.labels)
    print(f"saved {a.out} ({W} x {H}, {n} cells)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
