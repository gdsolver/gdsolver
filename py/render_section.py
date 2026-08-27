"""Draw one section of a level (objects + trajectory) on a labelled grid.

    python py/render_section.py --objects data/objrects_lv18.txt \\
        --dump data/solution_lv18_dp.txt.dump --xmin 24000 --xmax 24900 \\
        --out data/sec.png

Why this is needed: GD's picture is dark, the HUD hides the corners, and a
single frozen frame shows no motion. Both the geometry (objrects.txt) and the
trajectory (dump.csv) are already dumped, so drawing an honest picture directly
beats taking a screenshot.

The types are GameObject::m_objectType as recorded by the MOD:
  0=solid  2=hazard  4=portal family  20=trigger  7=decoration (not drawn)
Coordinates are CENTRES. A standard block is 30 units, so a solid is +/-15.
"""

from __future__ import annotations

import argparse
import sys
from dataclasses import dataclass
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

from PIL import Image, ImageDraw

from gdtas.draw import mono_font

BG = (18, 18, 22)
GRID = (38, 38, 46)
GRID5 = (70, 70, 84)
TEXT = (150, 150, 165)
SOLID = (150, 160, 175)
MOVING = (120, 200, 130)
HAZARD = (225, 70, 70)
PORTAL = (80, 150, 245)
TRIGGER = (240, 200, 60)
MARK = (60, 200, 255)


@dataclass
class Obj:
    id: int
    type: int
    x: float
    y: float
    w: float
    h: float
    grp: int


def read_objects(path: Path, xmin: float, xmax: float) -> list[Obj]:
    """objrects.txt (id,type,cx,cy,w,h[,groups]) carries the real sizes from GD's
    own getObjectRect(). objects.txt only has centres, and assuming 30x30 for
    those misreads decoration as solid mass. Prefer objrects.txt.
    """
    lines = path.read_text(encoding="utf-8-sig", errors="replace").splitlines()
    has_rects = bool(lines) and lines[0].startswith("id,type,cx,cy,w,h")
    out: list[Obj] = []
    for line in lines:
        if line.startswith("id,type"):
            continue
        p = line.split(",")
        if len(p) < 4:
            continue
        x = float(p[2])
        if x < xmin - 40 or x > xmax + 40:
            continue
        t = int(p[1])
        if t == 7:              # decoration: nothing but noise for this purpose
            continue
        w = float(p[4]) if has_rects and len(p) >= 6 else 30.0
        h = float(p[5]) if has_rects and len(p) >= 6 else 30.0
        grp = int(p[6]) if has_rects and len(p) >= 7 else 0
        out.append(Obj(int(p[0]), t, x, float(p[3]), w, h, grp))
    return out


def read_traj(path: Path, xmin: float, xmax: float, ymin: float, ymax: float,
              attempt: int) -> tuple[list[tuple[int, float, float]], int]:
    """dump.csv holds EVERY ATTEMPT. Overlaying them turns the picture into
    scribble, and the tick labels cover the geometry they are meant to explain.
    With attempt<0, automatically pick the attempt that got furthest right
    inside the window.
    """
    lines = path.read_text(encoding="utf-8-sig", errors="replace").splitlines()[1:]
    if attempt < 0:
        best_a, best_x = -1, -1e9
        for line in lines:
            p = line.split(",")
            if len(p) < 12 or p[11] != "0":
                continue
            x = float(p[3])
            if x <= xmax and x > best_x:
                best_x, best_a = x, int(p[1])
        attempt = best_a
        print(f"attempt {attempt} reached x={best_x:.0f} (furthest inside the window)")

    traj = []
    for line in lines:
        p = line.split(",")
        if len(p) < 12:
            continue
        if attempt >= 0 and int(p[1]) != attempt:
            continue
        # ALWAYS DROP DEAD ROWS. A corpse waiting for the reset keeps getting
        # ticked, and a fall off the course drags y down to -1e6. Keeping them
        # buries the real path and blows up the automatic y range, flattening
        # the picture.
        if p[11] != "0":
            continue
        x, y = float(p[3]), float(p[4])
        if x < xmin or x > xmax:
            continue
        if ymin > -1e8 and (y < ymin - 200 or y > ymax + 200):
            continue
        traj.append((int(p[2]), x, y))
    return traj, attempt


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--objects", required=True)
    ap.add_argument("--dump", default="")
    ap.add_argument("--xmin", required=True, type=float)
    ap.add_argument("--xmax", required=True, type=float)
    ap.add_argument("--ymin", type=float, default=-1e9)
    ap.add_argument("--ymax", type=float, default=1e9)
    ap.add_argument("--out", required=True)
    ap.add_argument("--max-pixels", type=int, default=1500)
    ap.add_argument("--mark-x", type=float, default=-1e9,
                    help="draw a vertical line here (the x it died at, say)")
    ap.add_argument("--mark-y", type=float, default=-1e9,
                    help="draw the player's box here")
    ap.add_argument("--attempt", type=int, default=-1,
                    help="-1 = pick the attempt that got furthest right in the window")
    ap.add_argument("--label-every", type=int, default=20)
    a = ap.parse_args(argv)

    xmin, xmax, ymin, ymax = a.xmin, a.xmax, a.ymin, a.ymax
    objs = read_objects(Path(a.objects), xmin, xmax)
    traj: list[tuple[int, float, float]] = []
    if a.dump and Path(a.dump).exists():
        traj, _ = read_traj(Path(a.dump), xmin, xmax, ymin, ymax, a.attempt)

    # y range: if not given, derive it from the objects inside the frame
    if ymin <= -1e8:
        ys = [o.y for o in objs] + [p[2] for p in traj]
        if not ys:
            print(f"nothing in x {xmin}..{xmax}")
            return 1
        ymin, ymax = min(ys) - 60, max(ys) + 60

    wU, hU = xmax - xmin, ymax - ymin
    scale = min(a.max_pixels / wU, a.max_pixels / hU)
    pad = 60
    W = int(wU * scale) + pad * 2
    H = int(hU * scale) + pad * 2

    def px(x: float) -> float:
        return pad + (x - xmin) * scale

    def py(y: float) -> float:
        return H - pad - (y - ymin) * scale          # y points up

    img = Image.new("RGB", (W, H), BG)
    d = ImageDraw.Draw(img)
    font = mono_font(13)
    font_s = mono_font(11)

    # Grid: one line = one GD block (30 units); every 5 blocks a bold line + label
    x = -(-int(xmin) // 30) * 30
    while x <= xmax:
        d.line([(px(x), py(ymax)), (px(x), py(ymin))],
               fill=GRID5 if x % 150 == 0 else GRID)
        if x % 150 == 0:
            d.text((px(x) - 18, H - pad + 4), str(int(x)), fill=TEXT, font=font_s)
        x += 30
    y = -(-int(ymin) // 30) * 30
    while y <= ymax:
        d.line([(px(xmin), py(y)), (px(xmax), py(y))],
               fill=GRID5 if y % 150 == 0 else GRID)
        if y % 150 == 0:
            d.text((6, py(y) - 7), str(int(y)), fill=TEXT, font=font_s)
        y += 30

    for o in objs:
        hx, hy = o.w / 2, o.h / 2
        bl, bt = px(o.x - hx), py(o.y + hy)
        bw = max(1.0, o.w * scale)
        bh = max(1.0, o.h * scale)
        if o.type == 0:
            # A solid with a group moves. Colour it differently so the still
            # image is not read as "this is permanently blocked"
            d.rectangle([bl, bt, bl + bw, bt + bh],
                        fill=MOVING if o.grp > 0 else SOLID)
        elif o.type == 2:
            # hazard: draw the inscribed diamond the corridor model uses
            cx, cy = px(o.x), py(o.y)
            r = max(hx, hy) * scale
            d.polygon([(cx, cy - r), (cx + r, cy), (cx, cy + r), (cx - r, cy)],
                      fill=HAZARD)
        elif o.type == 4:
            d.rectangle([bl, bt, bl + bw, bt + bh], fill=PORTAL)
        elif o.type == 20:
            d.ellipse([px(o.x) - 4, py(o.y) - 4, px(o.x) + 4, py(o.y) + 4],
                      fill=TRIGGER)
        else:
            d.rectangle([px(o.x) - 2, py(o.y) - 2, px(o.x) + 2, py(o.y) + 2],
                        fill=TRIGGER)

    # Trajectory last (drawn on top). Brighten it with tick so the direction reads
    if len(traj) > 1:
        n = len(traj)
        for i in range(1, n):
            f = i / n
            c = (int(60 + 195 * f), int(255 - 155 * f), 90)
            d.line([(px(traj[i - 1][1]), py(traj[i - 1][2])),
                    (px(traj[i][1]), py(traj[i][2]))], fill=c, width=3)
        # A mark every 20 ticks: this is what turns a path into MOTION
        for tick, tx, ty in traj:
            if tick % a.label_every:
                continue
            d.ellipse([px(tx) - 2.5, py(ty) - 2.5, px(tx) + 2.5, py(ty) + 2.5],
                      fill=(255, 255, 255))
            d.text((px(tx) + 4, py(ty) - 14), str(tick),
                   fill=(255, 255, 255), font=font_s)

    if a.mark_x > -1e8:
        d.line([(px(a.mark_x), py(ymax)), (px(a.mark_x), py(ymin))],
               fill=MARK, width=2)
        if a.mark_y > -1e8:
            d.rectangle([px(a.mark_x - 15), py(a.mark_y + 15),
                         px(a.mark_x - 15) + 30 * scale,
                         py(a.mark_y + 15) + 30 * scale], outline=MARK, width=2)

    d.text((6, 6),
           f"x {xmin:.0f}..{xmax:.0f}   y {ymin:.0f}..{ymax:.0f}   "
           f"grid=30u (bold=150u)   solid=grey hazard=red portal=blue trigger=yellow",
           fill=(255, 255, 255), font=font)

    out = Path(a.out)
    out.parent.mkdir(parents=True, exist_ok=True)
    img.save(out, "PNG")
    print(f"saved {out} ({W} x {H}) objects={len(objs)} trajPts={len(traj)}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
