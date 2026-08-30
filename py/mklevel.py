# -*- coding: utf-8 -*-
"""Generator for custom calibration maps.

WHY THEY EXIST: the 22 official levels do not yield samples of the physics
constants. Measured (2026-08-17, equivalent to py/sample_count): across the 22
verified solutions there are ONLY 5 slope-exit launches in total, each of them a
one-off. There are 0 launches for ball/robot/spider/UFO, and 0 for a
forward-moving cube at |m|=2. So when two constants "each measured at one point"
disagree, the corpus holds nothing to settle it (see how the m=2 anchor of
cubeExit turned into a per-mode split). A calibration map is A MEASURING RIG FOR
PRODUCING THOSE SAMPLES, not a level to be beaten (the official 22 stay as they
are, as the regression corpus).

The output is GD's level string in RAW (uncompressed) form. Compression is left
to GD's own ZipUtils on the MOD side (so the encoding is never guessed).

Take the objects you may place from the measured table in py/objpalette.py. All
the id/type pairs written here were confirmed against dumps of the 22 levels:

  id 1    type 0   30x30    basic block
  id 8    type 2   6x12     spike
  id 10   type 4   25x75    gravity portal (blue = flipped)
  id 11   type 3   25x75    gravity portal (yellow = normal)
  id 12   type 6   34x86    cube portal
  id 13   type 5   34x86    ship portal
  id 47   type 16  34x86    ball portal
  id 111  type 19  34x86    UFO portal
  id 660  type 26  34x86    wave portal
  id 745  type 27  34x86    robot portal
  id 1331 type 33  34x86    spider portal
  id 1933 type 41  33.5x85  swing portal
  id 45   type 14  44x92    size (mini)
  id 46   type 15  44x92    size (normal)
  id 1743 type 25  30x30    slope (|m|=1)
  id 1744 type 25  30x60    slope (|m|=2 / 0.5; the orientation depends on rot)
  id 1746 type 25  30x60    slope (same as above, another variant)
  id 1338 type 25  30x30    slope (the 30x30 one from lv21)

A SLOPE'S m AND sdir ARE FUNCTIONS OF rot/flip -- NEVER GUESS THE VALUES.
Load a single palette_probe map, have GD report them, and build the real
calibration rig from that table (this round trip doubles as a check on the
generator itself).

    python py/mklevel.py probe  --out data/rigs/calib_probe.lvl
    python py/mklevel.py slopes --out data/rigs/calib_slopes.lvl
"""
from __future__ import annotations

import argparse
import json
import math
from pathlib import Path

# ---- Object IDs (all from the measured table in objpalette.py) ----
BLOCK = 1
SPIKE = 8
# id 10 = NORMAL GRAVITY / id 11 = FLIPPED, exactly as leveldp's Obj::id note
# says; it was written the other way round at first. The symptom looked like
# this: right after unit 0 of the orb rig the player flipped and ran along the
# ceiling, passed straight through the floor-side portals (mode/size) of every
# later unit, and the fire value came out as "a flipped cube's jump, -11.180"
# for every orb.
GRAV_NORM, GRAV_FLIP = 10, 11
P_CUBE, P_SHIP, P_BALL, P_UFO = 12, 13, 47, 111
P_WAVE, P_ROBOT, P_SPIDER, P_SWING = 660, 745, 1331, 1933
# The size portals are TYPE 17 = NORMAL / 18 = MINI (leveldp's m_mini test).
# id 45/46 (type 14/15) are a different 44x92 object; placing them did not
# change the size at all.
SIZE_MINI, SIZE_NORM = 101, 99
SLOPE30, SLOPE60A, SLOPE60B, SLOPE30B = 1743, 1744, 1746, 1338

MODE_PORTAL = {"cube": P_CUBE, "ship": P_SHIP, "ball": P_BALL, "ufo": P_UFO,
               "wave": P_WAVE, "robot": P_ROBOT, "spider": P_SPIDER,
               "swing": P_SWING}

GRID = 30.0          # one GD block cell
# THE y IN THE LEVEL STRING IS world y - 90. Measured 2026-08-17 (this rig was
# loaded into GD and reported back via objrects): a block written as 75 in the
# string was reported by GD as cy=165, and a ramp written as 105 was reported as
# 195. That is,
#   world = string + 90
# The player stands at world y=105, and the ground (world 90) is solid across
# the whole level. This was the cause of the first instant deaths: a floor laid
# at string y=15 spans world 90..120 and blocked THE PLAYER'S SPAWN POSITION
# ITSELF. Conversely "laying it at y=75 clears the rig" was not a success
# either -- the run simply never touched that mid-air floor at world 150..180.
# From here on WRITE EVERYTHING IN world COORDINATES; obj() subtracts the offset
# as it writes.
Y_OFFSET = 90.0
GROUND_TOP = 90.0    # top of the ground = the surface the player stands on (world)
GROUND_Y = GROUND_TOP - GRID / 2   # centre for placing a floor block at that height


# The unit table. Filled in on every generation and written out to a
# .units.json alongside the .lvl. SO THE EXTRACTOR NEVER HAS TO GUESS "what
# condition does this x band represent".
UNITS: list[dict] = []
# The press windows. A rig THAT NEEDS TAPS, such as an orb rig, pushes
# (tick, 0|1) here and it is written to the .plan.txt (the rig and the plan must
# be generated together or the windows drift apart).
PLAN: list[tuple[int, int]] = []


def obj(oid: int, x: float, y: float, rot: float = 0.0,
        flip_x: bool = False, flip_y: bool = False,
        scale: float = 0.0, extra: dict[int, str] | None = None) -> str:
    """One object. The keys follow GD's level-string spec:
    1=id, 2=x, 3=y, 4=flipX, 5=flipY, 6=rotation, 32=scale.
    """
    # y arrives in world coordinates; subtract Y_OFFSET when writing (see note above)
    p = [f"1,{oid}", f"2,{x:g}", f"3,{y - Y_OFFSET:g}"]
    if flip_x:
        p.append("4,1")
    if flip_y:
        p.append("5,1")
    if rot:
        p.append(f"6,{rot:g}")
    if scale:
        p.append(f"32,{scale:g}")
    for k, v in (extra or {}).items():
        p.append(f"{k},{v}")
    return ",".join(p)


def header(start_mode: str = "cube", mini: bool = False, speed: int = 0,
           dual: bool = False) -> str:
    """The level's leading segment. THE STARTING STATE IS DECIDED HERE.

    kA2 = starting game mode (0 cube / 1 ship / 2 ball / 3 UFO / 4 wave /
    5 robot / 6 spider / 7 swing), kA3 = mini, kA4 = starting speed
    (0=1x, 1=0.5x, 2=2x, 3=3x, 4=4x), kA8 = dual, kA10 = 2player.
    kA6/kA7 are the background/ground texture numbers (cosmetic only).
    """
    m = {"cube": 0, "ship": 1, "ball": 2, "ufo": 3, "wave": 4, "robot": 5,
         "spider": 6, "swing": 7}[start_mode]
    return (f"kS38,1_40_2_125_3_255_11_255_12_255_13_255_4_-1_6_1000_7_1_15_1"
            f"_18_0_8_1|1_0_2_102_3_255_11_255_12_255_13_255_4_-1_6_1001_7_1"
            f"_15_1_18_0_8_1"
            f",kA13,0,kA15,0,kA16,0,kA14,,kA6,1,kA7,1,kA17,0,kA18,0"
            f",kA2,{m},kA3,{1 if mini else 0},kA4,{speed}"
            f",kA8,{1 if dual else 0},kA10,0,kA9,0,kA11,0")
# [2026-08-20] DO NOT WRITE kA22-kA45. THE RIG HEADER MUST STOP AT kA11: an
# official level's header (expanded from Resources/levels/7.txt) ends at kA11
# and has not a single key from kA22 on. The kA31..kA45 (mostly 1) that only the
# rig spelled out are the 2.2 compatibility flags, and THEY WERE CHANGING THE
# PHYSICS BETWEEN THE RIG AND THE OFFICIAL LEVELS:
#   rig ceilramp t=143..  flipped ship, no input = a constant +0.069/tick
#   lv7 t=5,793..         same conditions        = a constant +0.103/tick
# (both at sp0.9, vsize=1, gravityMod=1, dual=0 -- a different constant from
#  start to finish, not a vy threshold and not a frame effect).
# levelVersion/gameVersion were 1/0 for both the rig and the official levels, so
# the difference can only be in the level string's header.


# [2026-08-20] How far the run-up is paved. DO NOT RUN ON THE IMPLICIT WORLD
# GROUND. GD's world ground (top=90) is not treated like a solid block: when the
# player grows via a size portal while grounded, on top of a block it RESEATS ON
# THE SAME TICK (5 cases in the corpus), whereas on the world ground it happens
# THE NEXT TICK (same-tick reseating is a "push out of the penetration" action,
# so it cannot happen with nothing to push against).
# The "+6.000px for 1 tick" that always showed up in the rig's divergences was
# this, not a physics bug.
# cy=75 (top=90) is flush with the world ground, so paving it blocks no path.
PAVE_X = 60000.0


def floor_run(x0: float, x1: float, y: float = GROUND_Y,
              step: float = GRID) -> list[str]:
    """A floor filling [x0, x1]. GD'S GROUND (the y=0 surface) DOES HAVE A
    HITBOX, BUT A CALIBRATION RIG LAYS ITS OWN FLOOR AND PICKS ITS OWN HEIGHT."""
    out, x = [], x0
    while x <= x1:
        out.append(obj(BLOCK, x, y))
        x += step
    return out


def build_probe() -> str:
    """The palette verification map.

    IT MEASURES NO PHYSICS. Its purpose is to settle, in a single load, "if I
    place this id at this rot/flip, what type / w,h / m / slopeDir does GD
    report". It also checks the generator itself (are the coordinates what we
    intended) at the same time.

    Layout: one object every 30px, with a different row of y per kind. The
    player is never run (we only take the objrects dump), so collisions do not
    matter.
    """
    objs: list[str] = []
    objs += floor_run(0, 300)

    x = 400.0
    # Slopes: every combination of id x (rot, flipX, flipY). THIS IS THE MAIN
    # EVENT -- it is what fills in the m and slopeDir table.
    for oid in (SLOPE30, SLOPE30B, SLOPE60A, SLOPE60B):
        for rot in (0, 90, 180, 270):
            for fx, fy in ((False, False), (True, False), (False, True),
                           (True, True)):
                objs.append(obj(oid, x, 315.0, rot=rot, flip_x=fx, flip_y=fy))
                x += 90.0
    # Mode/size/gravity portals: check the reported type and box
    x2 = 400.0
    for oid in (P_CUBE, P_SHIP, P_BALL, P_UFO, P_WAVE, P_ROBOT, P_SPIDER,
                P_SWING, SIZE_MINI, SIZE_NORM, GRAV_FLIP, GRAV_NORM):
        objs.append(obj(oid, x2, 615.0))
        x2 += 90.0
    # Block and spike (the reference)
    x3 = 400.0
    for oid in (BLOCK, SPIKE):
        for rot in (0, 90, 180, 270):
            objs.append(obj(oid, x3, 915.0, rot=rot))
            x3 += 90.0
    return header() + ";" + ";".join(objs) + ";"


def build_slopes() -> str:
    """Slope calibration rig (v1: geometry only; per-mode sections wait until
    the probe table exists).

    Structure: flat run-up -> one slope -> flat landing area is one unit, laid
    out side by side. Units are 12 cells apart so one unit's launch cannot carry
    into the next.
    """
    objs: list[str] = []
    x = 0.0
    objs += floor_run(x, x + 300)
    x += 360
    for oid in (SLOPE30, SLOPE60A):
        for rot in (0, 90, 180, 270):
            # 4 cells of run-up
            objs += floor_run(x, x + 3 * GRID)
            x += 4 * GRID
            # The slope, sitting on the floor (30x30 centres at 45, 30x60 at 60)
            h = 60.0 if oid in (SLOPE60A, SLOPE60B) else 30.0
            objs.append(obj(oid, x, GROUND_Y + GRID / 2 + h / 2, rot=rot))
            objs.append(obj(BLOCK, x, GROUND_Y))
            x += GRID
            # 8 cells of landing area
            objs += floor_run(x, x + 7 * GRID)
            x += 12 * GRID
    return header() + ";" + ";".join(objs) + ";"


# ---- Placements settled by having GD report on the probe map (2026-08-17) ----
# The combinations that give "uphill with the solid side below" (= a floor ramp
# you can walk on). slopeIsCeiling treats {1,3,5,6} as ceiling, so sdir 0 and 7
# are floor.
#   |m|=0.5 : id1744 rot=0            -> w60 h30, sy cy-15 -> cy+15, sdir 0
#   |m|=1.0 : id1743 rot=0            -> w30 h30, sy cy-15 -> cy+15, sdir 0
#   |m|=2.0 : id1744 rot=90 + flipY   -> w30 h60, sy cy-30 -> cy+30, sdir 7
# The low end is aligned with the top of the floor, so cy is "floor top + half
# the height".
RAMP = {0.5: (SLOPE60A, 0.0, False, False, 60.0, 30.0),
        1.0: (SLOPE30, 0.0, False, False, 30.0, 30.0),
        2.0: (SLOPE60A, 90.0, False, True, 30.0, 60.0)}


def ramp_unit(x: float, mode: str, m: float, mini: bool, n_ramps: int,
              bury: float, floor_top: float = GROUND_TOP
              ) -> tuple[list[str], float]:
    """One unit of a calibration rig. SHAPED SO IT CAN BE CLEARED WITH ZERO INPUT.

    [mode/size portal][run-up][n ramps][plateau][gap], and the launch happens on
    the tick the player leaves the top of the ramp. The launch value can be read
    off the free flight over the plateau.

    `bury`: how many px of the ramp's low end are filled in from above with
    blocks. This shortens the ride so the ramp factor (a ramp-up that saturates
    in 0.1 s) can be isolated. 0 means the full length.
    Returns (list of objects, x of the next unit).
    """
    oid, rot, fx, fy, w, h = RAMP[m]
    objs: list[str] = []
    # Portals: place them at a height that overlaps the player's box (world 90..120)
    objs.append(obj(MODE_PORTAL[mode], x, floor_top + 15.0))
    objs.append(obj(SIZE_MINI if mini else SIZE_NORM, x + 2 * GRID,
                    floor_top + 15.0))
    # The run-up needs no blocks -- THE GROUND (world 90) IS SOLID ACROSS THE
    # WHOLE LEVEL. Laying a floor here would only block the player's path (see
    # the Y_OFFSET note above).
    run0 = 8
    xr = x + run0 * GRID
    # Climb n ramps joined end to end (GD only starts the ride timer on an
    # air->slope transition, so a contiguous chain accumulates ride ticks)
    bottom = floor_top
    for i in range(n_ramps):
        cy = bottom + h / 2.0
        # The ramp's box is w wide about cx. Align its low end with bottom.
        objs.append(obj(oid, xr + w / 2.0, cy, rot=rot,
                        flip_x=fx, flip_y=fy))
        # From the second ramp on there is a cavity underneath, so fill it
        # (to stop the player falling through)
        yy = floor_top + GRID / 2
        while yy < cy - h / 2.0 + 1.0:
            for k in range(int(w / GRID)):
                objs.append(obj(BLOCK, xr + GRID / 2 + k * GRID, yy))
            yy += GRID
        bottom += h
        xr += w
    # ALWAYS LEAVE THE SPACE BEYOND THE TOP END EMPTY. The launch rule fires on
    # the tick the player "leaves the top of the ramp", so joining a plateau at
    # the same height with no gap just lets the player walk across and no launch
    # ever happens (measured 2026-08-17: the plateau version cleared all 48
    # units with 0 launches). With a gap the player lands back on the ground, so
    # the arc can be measured.
    return objs, xr + 26 * GRID


def rampseam_unit(x: float, mode: str, m: float, mini: bool, ledge: int,
                  floor_top: float = GROUND_TOP) -> tuple[list[str], float]:
    """`ramp_unit` with a FLAT LEDGE butted against the ramp's top instead of a gap.

    The one thing `ramps` cannot see. Its ramps end in empty space, and there GD
    releases the ride's seat on the launch tick -- y takes the free step and the
    seat is thrown away (47/47 on that rig; the model does the same, and the
    lv19 t=254 measurement in step.hpp's launch branch is the same reading).

    lv16 t=13,264 disagrees: p2 launches (vy 0 -> 7.680, onGround 1 -> 0) and GD
    KEEPS THE SEAT, y = 457.150 = the ramp's surface at that x plus 9*sqrt(2).
    The only thing different about that site is what is beyond the ramp: a plain
    30x30 block whose top is exactly the ramp's top, so the surface continues.
    `ledge` is how many such blocks to lay (0 reproduces `ramps`).

    Zero input, like its sibling: the player rides up, and either walks onto the
    ledge or launches off it, and both are readable from the dump.
    """
    oid, rot, fx, fy, w, h = RAMP[m]
    objs: list[str] = []
    objs.append(obj(MODE_PORTAL[mode], x, floor_top + 15.0))
    objs.append(obj(SIZE_MINI if mini else SIZE_NORM, x + 2 * GRID,
                    floor_top + 15.0))
    xr = x + 8 * GRID
    # One ramp, its low end on the floor. Long enough to saturate the ride
    # counter (the factor is 1.0 past 24 ticks) so the launch VALUE is not the
    # variable under test here -- only what happens to y.
    cy = floor_top + h / 2.0
    objs.append(obj(oid, xr + w / 2.0, cy, rot=rot, flip_x=fx, flip_y=fy))
    top = floor_top + h            # the ramp's high edge
    xr += w
    # The ledge: blocks whose TOP FACE IS THE RAMP'S TOP, butted against it.
    for k in range(ledge):
        objs.append(obj(BLOCK, xr + GRID / 2 + k * GRID, top - GRID / 2))
    xr += ledge * GRID
    # ...and then empty space, so whatever happened is visible as free flight.
    return objs, xr + 26 * GRID


def build_rampseam() -> str:
    """Does a flat ledge past the ramp's top keep the ride's seat on the launch tick?

    4 ground modes x 3 gradients x normal/mini x ledge 0/1/3 = 72 units. Ledge 0
    is the `ramps` geometry and is the control: if it does not reproduce that
    rig's answer, the rig is wrong and not the model.
    """
    objs: list[str] = []
    x = 90.0
    for mode in ("cube", "ball", "robot", "spider"):
        for m in (1.0, 0.5, 2.0):
            for mini in (False, True):
                for ledge in (0, 1, 3):
                    x0 = x
                    u, x = rampseam_unit(x, mode, m, mini, ledge)
                    objs += u
                    UNITS.append({"x0": x0, "x1": x, "mode": mode, "m": m,
                                  "mini": int(mini), "ledge": ledge})
    objs += floor_run(0, PAVE_X)
    return header() + ";" + ";".join(objs) + ";"


def wavein_unit(x: float, mode: str, mini: bool, n: int = 4,
                floor_top: float = GROUND_TOP) -> tuple[list[str], float]:
    u"""Cross a WAVE PORTAL while GROUNDED, and come out the other side alive.

    lv20 t=17,116 is the corpus' one sample of this and it costs the level eight
    rounds: a UFO resting on the floor (onGround 1, vy 0) crosses a wave portal,
    and on that tick GD moves it by +0.029 and sets vy := -2.762, while the model
    moves it a full wave step (-1.613 = -dx) straight away. 1.643 px, and it never
    comes back. The sample cannot say whether that is about being grounded or
    about being the second body of a pair, because the first body was airborne.

    Building it on flat ground does not work: THE WAVE DESCENDS AS FAST AS IT
    ADVANCES, so a body that becomes a wave at floor+15 has thirteen pixels of
    headroom and is in the floor within a dozen ticks -- and one death ends the
    session. So the unit gives it a surface that falls away at the wave's own
    angle:

        [portals][run-up][n ramps up][flat top + WAVE PORTAL][n ramps down]

    The player climbs grounded, meets the portal standing on the flat top, and
    the descent that follows is a |m|=1 ramp -- exactly the wave's 45 degrees --
    so it flies just clear of it instead of into it. Zero input throughout.
    """
    oid, rot, fx, fy, w, h = RAMP[1.0]
    doid, drot, dfx, dfy, dw, dh = DOWNRAMP[1.0]
    objs: list[str] = []
    objs.append(obj(MODE_PORTAL[mode], x, floor_top + 15.0))
    objs.append(obj(SIZE_MINI if mini else SIZE_NORM, x + 2 * GRID,
                    floor_top + 15.0))
    xr = x + 8 * GRID
    bottom = floor_top
    for i in range(n):                       # climb
        objs.append(obj(oid, xr + w / 2.0, bottom + h / 2.0,
                        rot=rot, flip_x=fx, flip_y=fy))
        yy = floor_top + GRID / 2
        while yy < bottom + 1.0:
            for k in range(int(w / GRID)):
                objs.append(obj(BLOCK, xr + GRID / 2 + k * GRID, yy))
            yy += GRID
        bottom += h
        xr += w
    # NO FLAT TOP. One was tried and it is what breaks the rig: GD supports on
    # box overlap, so a half-15 body is still held up 15 px past the last block,
    # and it then free-falls while the descent runs away from it at 45 degrees.
    # Measured on that cut: the body leaves the top at x=555 and is STILL
    # AIRBORNE at the portal 33 px later (onGround 0, vy -5.4) -- the one thing
    # the rig exists to avoid. Crest straight from the climb into the descent
    # and the ride carries across the seam instead.
    top = bottom
    x_down = xr
    for i in range(n + 3):                   # and back down at the wave's angle
        bottom -= dh
        objs.append(obj(doid, xr + dw / 2.0, bottom + dh / 2.0,
                        rot=drot, flip_x=dfx, flip_y=dfy))
        yy = floor_top + GRID / 2
        while yy < bottom + 1.0:
            for k in range(int(dw / GRID)):
                objs.append(obj(BLOCK, xr + GRID / 2 + k * GRID, yy))
            yy += GRID
        xr += dw
        if bottom <= floor_top:
            break
    # THE PORTAL GOES OVER THE DESCENT, not over the flat top. A portal fires
    # when the boxes overlap, so it catches the player about 30 px before its
    # own centre -- and a body that becomes a wave has only 13.5 px of headroom
    # over whatever it was standing on (half 15 -> half 1.5). On the flat top
    # those 30 px of run are 30 px of descent into the blocks, every time; over
    # a |m|=1 descent the surface drops with it and the wave stays clear.
    # And well down it. Leaving the flat top the player FLOATS for half its own
    # width first -- GD supports on box overlap, so a half-15 body is still held
    # up 15 px past the last block -- and then falls until it catches the ramp.
    # Measured on the first cut of this rig: the top ends at x=540, the body
    # leaves it at 555 and is still airborne at 560. A portal one and a half
    # ramps in fired mid-fall and measured nothing. Two and a half is past the
    # re-acquisition and still 45 px short of the ground.
    objs.append(obj(P_WAVE, x_down + 2.5 * dw, top - 2.5 * dh + 15.0))
    return objs, xr + 20 * GRID


def build_wavein() -> str:
    u"""Crossing a wave portal off a ramp. 6 modes x normal/mini = 12 units.

    **IT DOES NOT YET MEASURE WHAT IT WAS BUILT FOR.** The target is a body that
    is GROUNDED on the tick a wave portal fires (lv20 t=17,116), and every unit
    here enters AIRBORNE -- the crest launches it, so it meets the portal at
    vy -9.0 with onGround 0. That is still a useful control (it is lv20's first
    body, which the model already matches), and the run is clean end to end, so
    it is kept rather than deleted.

    Two geometries were tried and both are recorded above, because between them
    they say why this is hard:

      * a flat top before the descent -- GD supports on box overlap, so the body
        floats 15 px past the last block and then free-falls while the descent
        runs away at 45 degrees. Airborne at the portal.
      * crest straight from climb to descent -- the ride's exit launch fires.
        Airborne again, and faster.

    Underneath both is one constraint: A WAVE DESCENDS AS FAST AS IT ADVANCES,
    and a body that becomes one keeps its position, so a half-15 body has 13.5 px
    of headroom over whatever it was standing on. The portal fires on box
    overlap, i.e. about 30 px before its own centre. So a flat surface under a
    wave portal is 30 px of run against 13.5 px of clearance -- the body is
    always inside the floor, and one death ends the session.

    What lv20 has and this does not is the DUAL GROUND LAYER: the second body
    stands on it with open space below, so it can become a wave and simply
    leave. The next cut should build the grounded side out of a dual band rather
    than out of blocks.
    """
    objs: list[str] = []
    x = 90.0
    for mode in ("cube", "ball", "robot", "spider", "ufo", "ship"):
        for mini in (False, True):
            x0 = x
            u, x = wavein_unit(x, mode, mini)
            objs += u
            UNITS.append({"x0": x0, "x1": x, "mode": mode, "mini": int(mini)})
    objs += floor_run(0, PAVE_X)
    return header() + ";" + ";".join(objs) + ";"


DUAL_CEIL = 380.0        # one continuous ceiling for every unit (see below)


def ceil_unit_dual(x: float, mode: str, m: float, mini: bool
                   ) -> tuple[list[str], float]:
    u"""A ceiling-ramp unit for THE SECOND BODY of a dual.

    `ceil_unit`'s sibling, and the differences are all forced by the pair:
      * NO GRAVITY PORTALS. In a dual the second body is already flipped, and it
        has to STAY flipped from one unit to the next -- `ceil_unit` flips and
        restores per unit because it drives a single player.
      * THE MODE/SIZE PORTALS SIT AT THE CEILING, not at the floor. A portal at
        floor height is taken by the first body and never reached by the second
        (measured on `dualmode`: 3,471 consecutive ticks with the halves in
        different modes), so putting them up here is what addresses the second.
      * ONE CEILING HEIGHT FOR EVERY UNIT. `ceil_y_for` raises it with the
        gradient, which is fine when each unit re-sticks its player; here the
        second body flies between units and would meet a different ceiling each
        time. 380 clears the deepest chain (|m|=2 over three ramps drops 180,
        ending at 200, well above the floor).
    """
    oid, rot, fx, fy, w, h = CEIL_RAMP[m]
    objs: list[str] = []
    ceil = DUAL_CEIL
    objs.append(obj(MODE_PORTAL[mode], x, ceil - 15.0))
    objs.append(obj(SIZE_MINI if mini else SIZE_NORM, x + 2 * GRID, ceil - 15.0))
    n_ramps = 3
    x_ramp = x + 20 * GRID
    xx = x + 4 * GRID + GRID / 2
    while xx < x_ramp:
        objs.append(obj(BLOCK, xx, ceil + GRID / 2))
        xx += GRID
    for i in range(n_ramps):
        cy = ceil - h / 2.0 - i * h
        objs.append(obj(oid, x_ramp + i * w + w / 2.0, cy,
                        rot=rot, flip_x=fx, flip_y=fy))
        yy = cy + h / 2.0 + GRID / 2
        while yy <= ceil + GRID / 2 + 1.0:
            for k in range(int(w / GRID)):
                objs.append(obj(BLOCK, x_ramp + i * w + GRID / 2 + k * GRID, yy))
            yy += GRID
    # Back to the flat ceiling at the original height (the `ceil_unit` trap: a
    # ceiling matching the ramp's far end crushes the player against the line).
    x_flat = x_ramp + n_ramps * w
    x_end = x_flat + 16 * GRID
    xx = x_flat + GRID / 2
    while xx < x_end:
        objs.append(obj(BLOCK, xx, ceil + GRID / 2))
        xx += GRID
    return objs, x_end + 14 * GRID


def build_ceilrampdual() -> str:
    u"""Does the SECOND BODY release the ride's seat on a ramp exit?

    The last thing lv16 t=13,264 can be. 95 launches across `ramps` and
    `rampseam` say the seat is released and y takes the free step, which is what
    the model does; `rampseamdual` then showed that being half of a pair changes
    nothing for the FIRST body (47 exits, identical tick for tick). What is left
    is the second body itself, and this puts it on the ramps.

    The first body walks the floor underneath and rides nothing, so its own
    column in the dump stays a control for the run's timing.

    Compare against `ceilramp`, which is the same geometry driven by a single
    flipped player.
    """
    objs: list[str] = []
    x = 90.0
    for mode in ("cube", "ball"):
        for m in (1.0, 0.5, 2.0):
            for mini in (False, True):
                x0 = x
                u, x = ceil_unit_dual(x, mode, m, mini)
                objs += u
                UNITS.append({"x0": x0, "x1": x, "mode": mode, "m": m,
                              "mini": int(mini)})
    objs += floor_run(0, PAVE_X)
    return header(dual=True) + ";" + ";".join(objs) + ";"


def build_rampseamdual() -> str:
    """`rampseam` again, with the level STARTED IN DUAL. Halves the question.

    lv16 t=13,264 is the one ramp exit in the corpus where GD keeps the ride's
    seat, and 95 launches across `ramps` and `rampseam` say it should not. What
    that site has and those rigs do not is that the rider is HALF OF A PAIR --
    so ask whether that alone changes anything, before building the harder rig
    where the SECOND body is the one on the ramp.

    Only the header differs. The first body still rides every ramp exactly as it
    does in `rampseam` (the portals sit at floor height, so in a dual the first
    body takes them and the second never reaches them -- measured on `dualmode`).
    The second body flips away upward and is held by the band, out of the way.

    So: p1's exits here against p1's exits there is a clean one-variable A/B.
      * they differ  -> being in a dual is what does it, and the second body is
                        a red herring
      * they match   -> it is the second body specifically, and the next rig has
                        to put IT on the ramp
    """
    objs: list[str] = []
    x = 90.0
    for mode in ("cube", "ball", "robot", "spider"):
        for m in (1.0, 0.5, 2.0):
            for mini in (False, True):
                for ledge in (0, 1, 3):
                    x0 = x
                    u, x = rampseam_unit(x, mode, m, mini, ledge)
                    objs += u
                    UNITS.append({"x0": x0, "x1": x, "mode": mode, "m": m,
                                  "mini": int(mini), "ledge": ledge})
    objs += floor_run(0, PAVE_X)
    return header(dual=True) + ";" + ";".join(objs) + ";"


def build_ramps() -> str:
    """Calibration rig for slope-exit launches. CLEARS EVERY UNIT WITH ZERO INPUT.

    The 22 official solutions yield 5 launch samples in total (each a one-off,
    0 for ball/robot/spider, and 0 for a forward-moving cube at |m|=2). This rig
    is 4 ground modes x 3 values of |m| x normal/mini x 2 ride lengths = 48
    units, producing 48 samples from a single run.
    The flight modes (ship/UFO/wave) need input, so they get their own rig.
    """
    objs: list[str] = []
    x = 90.0
    for mode in ("cube", "ball", "robot", "spider"):
        for m in (1.0, 0.5, 2.0):
            for mini in (False, True):
                for n, bury in ((3, 0.0), (1, 0.0)):
                    x0 = x
                    u, x = ramp_unit(x, mode, m, mini, n, bury)
                    objs += u
                    UNITS.append({"x0": x0, "x1": x, "mode": mode, "m": m,
                                  "mini": int(mini), "ramps": n,
                                  "bury": bury})
    # [2026-08-20] Lay the floor LAST = give it a larger uid than the portals.
    # GD's collision pass runs in uid order, and only a solid processed AFTER
    # the size portal sees the "grown box" and pushes the player out on the same
    # tick. Placing the floor first makes the reseat happen the next tick, which
    # disagrees with the corpus (lv12/lv15/lv19/lv22 are all same-tick). In the
    # official levels the floor has a later uid than the player's path.
    objs += floor_run(0, PAVE_X)
    return header() + ";" + ";".join(objs) + ";"


# ---- Orbs / pads (from the measured table in objpalette.py) ----
ORB = {"yellow": (36, 11), "pink": (141, 12), "blue": (84, 13),
       "green": (1022, 29), "red": (1333, 35), "black": (1330, 32),
       "dash": (1704, 37), "gravdash": (1751, 38), "spider": (3004, 43)}
PAD = {"yellow": (35, 8), "pink": (140, 9), "blue": (67, 10)}

DX_1X = 1.29825044   # x increment at 1x speed (measured)


# The (tick, x) correspondence table built from a measured dump passed via
# --xmap. THIS IS THE ONE THAT MATTERS.
# The naive formula x(t) = dx*(t-1) drifts on a long rig: measured (orbedge rig,
# 105,592 ticks / x=136,000) the average was 1.288/tick, 104px BEHIND the
# prediction (836 ticks do not advance x at all = a few ticks per unit). On a
# rig whose press windows are specified in ticks, that lag translates directly
# into "the press did not happen at the intended x".
# Procedure: run it once, take the dump, and regenerate passing that dump to
# --xmap.
XMAP: list[tuple[int, float]] = []


def tick_at(x: float, dx: float = DX_1X) -> int:
    """The tick at which the player reaches that x.

    If XMAP exists, look it up FROM THE MEASUREMENT (the first tick at or past
    x). Otherwise fall back on the approximation x(t) = dx*(t-1) (good enough on
    a short rig).
    """
    if XMAP:
        lo, hi = 0, len(XMAP) - 1
        while lo < hi:
            mid = (lo + hi) // 2
            if XMAP[mid][1] < x:
                lo = mid + 1
            else:
                hi = mid
        return XMAP[lo][0]
    return int(x / dx) + 1


def load_xmap(dump: Path) -> None:
    """Load (tick, x) from a dump.csv (x is monotonic, so bisection works)."""
    import csv as _csv
    with dump.open(newline="", encoding="utf-8-sig", errors="replace") as f:
        for r in _csv.DictReader(f):
            try:
                XMAP.append((int(r["tick"]), float(r["x"])))
            except (ValueError, KeyError):
                continue
    XMAP.sort()


def orb_unit(x: float, kind: str, mode: str, mini: bool, drop: float
             ) -> tuple[list[str], list[tuple[int, int]], float]:
    """A calibration unit for a single orb. MAKE THE TAP HAPPEN WHILE FALLING.

    Pressing while grounded fires the mode's own action first (a jump, for a
    cube), so the shape has the player step off a high platform and touch the
    orb IN MID-AIR. That way the press can only mean the orb firing, and the vy
    at contact can also be chosen via the platform height.

    Returns (list of objects, press window [(tick, 0|1)], next x).
    """
    oid, _ = ORB[kind]
    objs: list[str] = []
    # PLACE IT AT GROUND LEVEL. The first design was "step off a high platform
    # and tap in mid-air", but the platform became a floating island out of
    # reach of the ground (world 90); the player ran underneath it, never
    # touched the portal or the orb, and the press became A PLAIN JUMP (all 72
    # units read incoming -10.852 -> 0 = a landing).
    # Even when tapping on the ground, an overlapping orb takes priority and
    # fires, so this measures the raw value at incoming vy=0. Varying the
    # incoming vy is the job of a separate rig.
    y = GROUND_TOP + 15.0
    ceil = GROUND_TOP + 210.0            # the landing surface when flipped (world)
    objs.append(obj(MODE_PORTAL[mode], x, y))
    objs.append(obj(SIZE_MINI if mini else SIZE_NORM, x + 2 * GRID, y))
    x_orb = x + 8 * GRID
    objs.append(obj(oid, x_orb, y))
    x_next = x_orb + 40 * GRID
    # PUT A CEILING UP. The blue orb and the gravity dash flip gravity, so
    # without a ceiling the player "falls" upward and dies off-screen (measured:
    # the run ended on the blue mini unit).
    xx = x - GRID
    while xx <= x_next:
        objs.append(obj(BLOCK, xx, ceil + GRID / 2))
        xx += GRID
    # Restore normal gravity at the end of the unit. They go on both the floor
    # side and the ceiling side so a player still flipped and running along the
    # ceiling passes through one for certain.
    objs.append(obj(GRAV_NORM, x_next - 4 * GRID, y))
    objs.append(obj(GRAV_NORM, x_next - 4 * GRID, ceil - 15.0))
    # The press window opens ONLY ONCE THE BOXES OVERLAP. Pressing before that
    # produces the mode's own jump, so it starts at the x where the orb (36x36)
    # and the player (half-width 15) overlap.
    # THE WINDOW MUST START AFTER THE OVERLAP BEGINS FOR BOTH SIZES. At -28 it
    # is inside for normal (half-width 15, overlap starts at -33) but 1px early
    # for mini (half-width 9, -27), and the press on that tick became THE MINI'S
    # OWN JUMP, 8.944, instead of the orb (measured: the blue/green/red/black
    # mini units all came out 8.944 flip0). -20 is inside for both.
    t_press = tick_at(x_orb - 20.0)
    t_rel = tick_at(x_orb + 10.0)
    return objs, [(t_press, 1), (t_rel, 0)], x_next, t_press, t_rel


# ---- Placements for ceiling ramps (solid above the line). Read off the 64
# ---- combinations of the probe rig.
# DOWNHILL ceiling ramps (the line descends as x advances = same shape as lv18
# uid1807). The line runs from cy+h/2 at the left edge to cy-h/2 at the right.
# For sdir, {1,3,5,6} are ceiling.
#   |m|=0.5 : id1744 rot=0  flipY        -> w60 h30, sdir 1
#   |m|=1.0 : id1743 rot=0  flipY        -> w30 h30, sdir 1
#   |m|=2.0 : id1744 rot=90 flipX+flipY  -> w30 h60, sdir 5
CEIL_RAMP = {0.5: (SLOPE60A, 0.0, False, True, 60.0, 30.0),
             1.0: (SLOPE30, 0.0, False, True, 30.0, 30.0),
             2.0: (SLOPE60A, 90.0, True, True, 30.0, 60.0)}

# Underside of the flat ceiling (world). RAISE IT BY THE RAMP'S TOTAL DROP -- a
# 3-ramp chain at |m|=2 descends 180px, so a fixed 255 would put the far end
# through the ground (90) and kill the player.
def ceil_y_for(h: float, n: int = 3) -> float:
    return 200.0 + n * h


def ceil_unit(x: float, mode: str, m: float, mini: bool
              ) -> tuple[list[str], float]:
    u"""One ceiling-ramp unit. Clears with ZERO INPUT.

    [mode/size portal][gravity flip][flat ceiling][downhill ramp][flat ceiling]
    [gravity restore]

    Flipping gravity sticks every mode to the ceiling with no input (ship/UFO
    accelerate into it; cube/ball/robot "land" on it). Sliding along the ramp's
    underside from there is the same shape as lv18 t=3,007. What is measured is
    the CONTACT OFFSET (line - player y) and the velocity on leaving the ramp.
    """
    oid, rot, fx, fy, w, h = CEIL_RAMP[m]
    objs: list[str] = []
    objs.append(obj(MODE_PORTAL[mode], x, GROUND_TOP + 15.0))
    objs.append(obj(SIZE_MINI if mini else SIZE_NORM, x + 2 * GRID,
                    GROUND_TOP + 15.0))
    objs.append(obj(GRAV_FLIP, x + 4 * GRID, GROUND_TOP + 15.0))
    n_ramps = 3
    ceil = ceil_y_for(h, n_ramps)
    # Flat ceiling (underside at ceil). The run-up must be long enough for the
    # ship/wave to finish climbing
    x_ramp = x + 20 * GRID
    xx = x + 4 * GRID + GRID / 2
    while xx < x_ramp:
        objs.append(obj(BLOCK, xx, ceil + GRID / 2))
        xx += GRID
    # Descend n_ramps ramps joined end to end (buy ride ticks so the ramp
    # factor saturates)
    for i in range(n_ramps):
        cy = ceil - h / 2.0 - i * h
        objs.append(obj(oid, x_ramp + i * w + w / 2.0, cy,
                        rot=rot, flip_x=fx, flip_y=fy))
        # Fill from the top of the ramp box up to the ceiling so the solid connects
        yy = cy + h / 2.0 + GRID / 2
        while yy <= ceil + GRID / 2 + 1.0:
            for k in range(int(w / GRID)):
                objs.append(obj(BLOCK, x_ramp + i * w + GRID / 2 + k * GRID, yy))
            yy += GRID
    # Past the ramp, GO BACK TO A FLAT CEILING AT THE ORIGINAL HEIGHT.
    # A trap stepped on twice: put a low ceiling matching the line at the ramp's
    # far end, and the top edge of the descending player's box
    # (line - offset + pH) is still above that ceiling, so the player is crushed
    # between the line and the ceiling and dies instantly. After leaving the
    # ramp the player climbs again under flipped gravity, so THE CEILING SHOULD
    # SIMPLY CONTINUE AT THE ORIGINAL HEIGHT (aligning the block's left edge
    # with the ramp's right edge is also mandatory -- aligning centres sticks
    # out 15px too early).
    x_flat = x_ramp + n_ramps * w
    x_end = x_flat + 16 * GRID
    xx = x_flat + GRID / 2
    while xx < x_end:
        objs.append(obj(BLOCK, xx, ceil + GRID / 2))
        xx += GRID
    # Restore gravity (at a height that hits the player re-stuck to the
    # original ceiling)
    objs.append(obj(GRAV_NORM, x_flat + 12 * GRID, ceil - 15.0))
    return objs, x_end + 14 * GRID


def build_ceilramp() -> str:
    u"""Calibration rig for ceiling ramps. CLEARS EVERY UNIT WITH ZERO INPUT.

    Across the 22 official levels there is exactly one sample of "a flipped
    flying body riding a ceiling ramp", lv18 t=3,007, and it is a single point
    at |m|=1. It shows the contact offset is off (GD 12.728 vs model 18), but
    the gradient dependence cannot be separated out of it.

    5 modes x 3 values of |m| x normal/mini = 30 units from a single run.

    [2026-08-21] robot/spider/swing added AT THE END (8 modes x 3 x 2 = 48
    units). The hanging-side contact-point rule (sampleAt's split between
    centre/contactPt/box edge) had only ever been measured on this rig's
    ship/cube/ball/UFO, so measure it across the whole surface before the
    whitelist grows one more mode at a time. They go last so that if a new mode
    dies the existing 30 survive.
    """
    objs: list[str] = []
    x = 90.0
    # WAVE GOES LAST (measured 2026-08-21: wave takes GD itself down on its own
    # units of this rig -- in the original order the run stopped at unit 24 and
    # nothing from 25 on was ever measured)
    for mode in ("ship", "cube", "ball", "ufo",
                 "robot", "spider", "swing", "wave"):
        for m in (1.0, 0.5, 2.0):
            for mini in (False, True):
                x0 = x
                u, x = ceil_unit(x, mode, m, mini)
                objs += u
                UNITS.append({"x0": x0, "x1": x, "mode": mode, "m": m,
                              "mini": int(mini),
                              "x_ramp": x0 + 20 * GRID,
                              "ramp_w": CEIL_RAMP[m][4],
                              "ramps": 3,
                              "ceil_y": ceil_y_for(CEIL_RAMP[m][5], 3)})
    # Floor LAST = a larger uid than the portals (the lesson of build_ramps
    # L305). Laying it first makes the mini->normal reseat happen the next tick
    # (GD's collision pass runs in uid order), producing a fake 6.000px x 1 tick
    # divergence against the model's same-tick reseat at every unit boundary
    objs += floor_run(0, PAVE_X)
    return header() + ";" + ";".join(objs) + ";"


def ridemode_unit(x: float, mode: str, target: str, m: float, mini: bool,
                  ceiling: bool, ride_ticks: float | None = None
                  ) -> tuple[list[str], float, dict]:
    u"""A MODE PORTAL PLACED MID-RIDE, on a floor ramp or on a ceiling one.

    lv20's most expensive wall (8 of 40 rounds) is one tick: the second body
    rides a 45-degree slope upside-down, meets the wave portal, and GD steps it
    +0.029 while the model steps it -1.614 -- the ride's own step, m*dx. The
    model has the free-step release for two cases only, both written from a
    single site:

        oldMode == 7 && s.onSlope                 (a swing leaving a ride)
        oldMode != wantMode && s.ceilT > 0 && !s.onSlope   (a ceiling press)

    and this body is neither. Widening the second gate to cover it looked
    obvious until the corpus was asked: of the three ticks in the 22 solutions
    where a mode portal fires with the ride running, ALL THREE HAVE GD TAKING
    THE RIDE'S STEP (lv16 t=9,879 / t=15,537, lv18 t=9,295 -- ship to cube on a
    floor ramp, |dy - m*dx| <= 0.001). Three samples that say the opposite of
    the one site is not a rule either way, and they differ in more than one
    thing at once, so this rig moves ONE: the side the ramp carries from.

        floor   [mode][size][run-up][3 ramps up ....P.... ][gap]
        ceiling [mode][size][flip][flat ceiling][3 ramps down ....P....][flat]

    P is a portal to `target`, placed 2.5 ramps in so it fires with the ride
    long since saturated, at the height the ride itself puts the body
    (`ride_seat`). Both sides clear with zero input, which is what makes the
    tick readable: whatever y moves by on the portal tick is the rule, not the
    plan.
    """
    oid, rot, fx, fy, w, h = (CEIL_RAMP if ceiling else RAMP)[m]
    objs: list[str] = []
    # EACH HEAD PORTAL IS LAID TWICE, at 105 and at 195. A unit that keeps the
    # player high for a long stretch leaves the CAMERA BAND raised behind it:
    # measured on cut 3, after the ceiling unit at x=39,000 the band floor was
    # pmin=150, so the body came down onto the INVISIBLE FLOOR at y=165 and
    # every later unit's portal -- sitting at 105, below the band -- was out of
    # reach. Six units in a row then measured nothing at all while the run still
    # looked clean. A portal at 195 catches the body in that state; one at 105
    # catches it in the ordinary one.
    for py in (GROUND_TOP + 15.0, GROUND_TOP + 105.0):
        objs.append(obj(MODE_PORTAL[mode], x, py))
        objs.append(obj(SIZE_MINI if mini else SIZE_NORM, x + 2 * GRID, py))
    if ceiling:
        objs.append(obj(GRAV_FLIP, x + 4 * GRID, GROUND_TOP + 15.0))
    n_ramps = 3
    ceil = ceil_y_for(h, n_ramps)
    x_ramp = x + (20 if ceiling else 8) * GRID
    if ceiling:
        xx = x + 4 * GRID + GRID / 2
        while xx < x_ramp:
            objs.append(obj(BLOCK, xx, ceil + GRID / 2))
            xx += GRID
    for i in range(n_ramps):
        if ceiling:
            cy = ceil - h / 2.0 - i * h
            objs.append(obj(oid, x_ramp + i * w + w / 2.0, cy,
                            rot=rot, flip_x=fx, flip_y=fy))
            yy = cy + h / 2.0 + GRID / 2
            while yy <= ceil + GRID / 2 + 1.0:
                for k in range(int(w / GRID)):
                    objs.append(obj(BLOCK,
                                    x_ramp + i * w + GRID / 2 + k * GRID, yy))
                yy += GRID
        else:
            cy = GROUND_TOP + i * h + h / 2.0
            objs.append(obj(oid, x_ramp + i * w + w / 2.0, cy,
                            rot=rot, flip_x=fx, flip_y=fy))
            yy = GROUND_TOP + GRID / 2
            while yy < cy - h / 2.0 + 1.0:
                for k in range(int(w / GRID)):
                    objs.append(obj(BLOCK,
                                    x_ramp + i * w + GRID / 2 + k * GRID, yy))
                yy += GRID
    # THE PORTAL FIRES ABOUT 32 px BEFORE ITS OWN CENTRE (box overlap: the mode
    # portal's own half-width 17 plus the player's 15), so the height has to be
    # the seat AT THE FIRING POINT, not under the centre -- on a 45-degree ramp
    # those are 32 px apart, more than the portal's own half-height.
    #
    # `ride_ticks` places it by HOW OLD THE RIDE IS instead, which is the whole
    # point of the third cut. The ride's own start is measured, not assumed: on
    # the ceiling side the body leaves the flat ceiling at x_ramp - 5.9 (rig
    # pass 2, unit 28, t=30,385) -- GD samples the line 6.2 px AHEAD of the
    # centre, so the descent begins before the ramp's own left edge.
    if ride_ticks is None:
        port_cx = x_ramp + 2.5 * w
    else:
        port_cx = x_ramp - 5.9 + ride_ticks * DX_1X + 32.0
    travel = max(0.0, port_cx - 32.0 + 6.2 - x_ramp)
    i_at = min(n_ramps - 1, int(travel / w))
    surf = (ceil - i_at * h - (travel - i_at * w) * (h / w)) if ceiling else \
           (GROUND_TOP + i_at * h + (travel - i_at * w) * (h / w))
    # The seat is the line at that sample point minus the player's own half --
    # NOT half*sqrt(1+m^2). Measured on this rig (unit 28, a flipped UFO on the
    # |m|=1 ceiling chain): line 241.66 - 15 = 226.66, GD's row to the digit,
    # and the same formula lands lv20's p2 at 295.76 exactly.
    port_cy = surf - (9.0 if mini else 15.0) if ceiling else \
              surf + (9.0 if mini else 15.0)
    objs.append(obj(MODE_PORTAL[target], port_cx, port_cy))
    x_flat = x_ramp + n_ramps * w
    if ceiling:
        # Back to the flat ceiling at the ORIGINAL height -- a ceiling matching
        # the ramp's far end crushes the player against the line (`ceil_unit`'s
        # trap, stepped on twice).
        x_end = x_flat + 16 * GRID
        xx = x_flat + GRID / 2
        while xx < x_end:
            objs.append(obj(BLOCK, xx, ceil + GRID / 2))
            xx += GRID
        objs.append(obj(GRAV_NORM, x_flat + 12 * GRID, ceil - 15.0))
        nxt = x_end + 14 * GRID
    else:
        # Nothing past the top end: the launch needs somewhere to land.
        nxt = x_flat + 26 * GRID
    return objs, nxt, {"x_ramp": x_ramp, "ramp_w": w, "ramp_h": h,
                       "port_cx": port_cx, "port_cy": port_cy,
                       "ceil_y": ceil if ceiling else 0.0}


def build_ridemode() -> str:
    u"""Does a mode portal end a ramp ride on its own tick? One variable: which
    side the ramp carries from.

    Floor units go FIRST and use the four ground modes only -- a flight mode
    needs input to stay on a climbing ramp, which is why `ramps` has never had
    one. The ceiling side sticks every mode to the underside with a gravity
    flip and no input at all (`ceilramp`'s whole point), so the flight modes are
    measurable there, and that is where lv20's own body is: a UFO pressed along
    a descending ramp, upside down.

    `wave` and `swing` are left out on purpose. The wave takes GD itself down
    on `ceilramp`'s own units (2026-08-21), and the swing is the one case the
    model already has a measured rule for.

    |m| = 1 throughout: lv20's ramp is 45 degrees and the question is the side,
    not the gradient. If the side turns out to matter, the gradient is the next
    rig, not another axis of this one.

    [2026-08-29, second cut] THE SIDE DOES NOT MATTER: all 20 units of the first
    cut take the ride's step, floor and ceiling alike, every mode, both sizes.
    So the model is right about lv20's tick as far as this rig can see, and what
    is left of the difference is the ONE thing every unit above holds fixed --
    THE NEW MODE'S BOX IS THE SAME SIZE AS THE OLD ONE. lv20's portal is a wave
    portal: half 15 becomes half 5, and a body that was pressed against a
    surface is suddenly 10 px clear of it with nothing to ride.

    The model has the other direction of exactly this (a mode portal whose half
    GROWS re-seats the player on the same tick, measured on lv22's spider pairs)
    and nothing at all for the shrink, on the stated grounds that "shrinking has
    nothing to push against" -- true of the position, silent about the contact.

    So this cut adds two arms:
      * target `spider` -- 15 to 13.5, the smallest shrink there is, and safe
        on a ramp. On a 45-degree ramp 1.5 px of half is 2.1 px of seat, so the
        ride step (1.27) and a free one are still far apart.
      * one `ufo -> wave` unit, ceiling, LAST -- lv20's own shape (a flipped
        flight body riding a descending ramp into a wave portal) and the biggest
        shrink available. It cannot survive: a flipped wave with no input flies
        into the ceiling it just left, ~3 ticks after the portal. That is why it
        is last -- ONE DEATH ENDS THE RUN, and everything after it is lost.
    """
    objs: list[str] = []
    x = 90.0
    m = 1.0
    # (ceiling, from, to, sizes)
    plan: list[tuple[bool, str, str, tuple[bool, ...]]] = []
    both = (False, True)
    # (a) the half does not change -- the control
    for mode, target in (("cube", "ball"), ("ball", "cube"),
                         ("robot", "cube"), ("spider", "cube")):
        plan.append((False, mode, target, both))
    for mode, target in (("ship", "ball"), ("cube", "ball"), ("ball", "cube"),
                         ("ufo", "ball"), ("robot", "cube"),
                         ("spider", "cube")):
        plan.append((True, mode, target, both))
    # (b) the half shrinks 15 -> 13.5
    for mode in ("cube", "ball"):
        plan.append((False, mode, "spider", both))
    for mode in ("ball", "ufo"):
        plan.append((True, mode, "spider", both))
    # (c) HOW OLD THE RIDE IS when the portal fires. Everything above meets it
    # with the ride saturated (38 ticks, measured), and lv20's second body meets
    # it ONE TICK after landing on the ramp -- which is the only difference left
    # between GD's two answers. `ball` keeps the half unchanged, so these live.
    ages: tuple[float, ...] = (1.0, 2.0, 3.0, 5.0, 9.0, 15.0)
    # (d) ...and 15 -> 5, which is lv20's, and which dies. LAST.
    for ceiling, mode, target, sizes in plan:
        for mini in sizes:
            x0 = x
            u, x, info = ridemode_unit(x, mode, target, m, mini, ceiling)
            objs += u
            UNITS.append({"x0": x0, "x1": x, "mode": mode, "target": target,
                          "m": m, "mini": int(mini), "ceiling": int(ceiling),
                          "age": -1, **info})
    for age in ages:
        x0 = x
        u, x, info = ridemode_unit(x, "ufo", "ball", m, False, True,
                                   ride_ticks=age)
        objs += u
        UNITS.append({"x0": x0, "x1": x, "mode": "ufo", "target": "ball",
                      "m": m, "mini": 0, "ceiling": 1, "age": age, **info})
    x0 = x
    u, x, info = ridemode_unit(x, "ufo", "wave", m, False, True)
    objs += u
    UNITS.append({"x0": x0, "x1": x, "mode": "ufo", "target": "wave",
                  "m": m, "mini": 0, "ceiling": 1, "age": -1, **info})
    # Floor LAST = a larger uid than the portals (build_ramps L305).
    objs += floor_run(0, PAVE_X)
    return header() + ";" + ";".join(objs) + ";"


def ridedrop_unit(x: float, mode: str, target: str, mini: bool,
                  port_x: float | None = None, n_ramps: int = 6,
                  top: float = 375.0) -> tuple[list[str], float, dict]:
    u"""ARRIVE AT THE RAMP BY PENETRATING IT, not by sliding onto it.

    `ridemode` asked GD 39 times whether a mode portal ends a ramp ride and got
    "no" every time, across the ramp's side, six modes, both sizes, a box that
    stays the same or shrinks to a fifth, a ride 0 to 40 ticks old, and both
    halves of a dual. lv20 t=17,117 still does the opposite. The one thing all
    39 hold fixed is HOW THE BODY GOT ONTO THE RAMP: every one of them slides on,
    following the surface down, and its first contact tick moves LESS than a full
    ride step (-0.354 measured). lv20's second body is born inside the ramp by
    the dual portal and pushed out of it -- its first contact tick moves -2.583,
    MORE than the ride step.

    So this unit arrives the other way. A gravity flip under an open sky lets the
    body fall upward for ~95 ticks while a ceiling ramp descends to meet it, and
    they close at about 4 px/tick -- the same order as lv20's 2.583. The body
    cannot follow that surface onto itself; it goes through and is pushed back.

        [mode][size][flip]......(open, the body accelerates upward)......
                          [6 ceiling ramps descending from `top`][flat][restore]

    `port_x` is the mid-ride mode portal. It is left off on the first pass --
    where the body meets the ramp is a measurement, not a calculation -- and
    placed on the second from the contact tick the dump reports.
    """
    oid, rot, fx, fy, w, h = CEIL_RAMP[1.0]
    objs: list[str] = []
    for py in (GROUND_TOP + 15.0, GROUND_TOP + 105.0):
        objs.append(obj(MODE_PORTAL[mode], x, py))
        objs.append(obj(SIZE_MINI if mini else SIZE_NORM, x + 2 * GRID, py))
    objs.append(obj(GRAV_FLIP, x + 6 * GRID, GROUND_TOP + 15.0))
    x_ramp = x + 8 * GRID
    for i in range(n_ramps):
        cy = top - h / 2.0 - i * h
        objs.append(obj(oid, x_ramp + i * w + w / 2.0, cy,
                        rot=rot, flip_x=fx, flip_y=fy))
        # Fill ABOVE the diagonal only. The body comes up from underneath, so
        # the diagonal has to be the lowest solid thing it can meet.
        yy = cy + h / 2.0 + GRID / 2
        while yy <= top + GRID / 2 + 1.0:
            for k in range(int(w / GRID)):
                objs.append(obj(BLOCK, x_ramp + i * w + GRID / 2 + k * GRID, yy))
            yy += GRID
    x_flat = x_ramp + n_ramps * w
    low = top - n_ramps * h
    x_end = x_flat + 16 * GRID
    xx = x_flat + GRID / 2
    while xx < x_end:
        objs.append(obj(BLOCK, xx, low + GRID / 2))
        xx += GRID
    port_cy = 0.0
    if port_x is not None:
        # The seat is line(centre + 6.21) - half (measured on `ridemode`), and
        # the portal fires ~32 px before its own centre.
        s = port_x - 32.0 + 6.21
        line = top - max(0.0, min(s - x_ramp, n_ramps * h))
        port_cy = line - (9.0 if mini else 15.0)
        objs.append(obj(MODE_PORTAL[target], port_x, port_cy))
    objs.append(obj(GRAV_NORM, x_flat + 12 * GRID, low - 15.0))
    return objs, x_end + 14 * GRID, {"x_ramp": x_ramp, "ramp_w": w,
                                     "ramp_h": h, "top": top,
                                     "port_cx": port_x or 0.0,
                                     "port_cy": port_cy, "ceil_y": low}


# WHERE EACH MODE FIRST TOUCHES THE RAMP, relative to the unit's own x0, and
# what its arrival looks like. Measured on the first pass (the same geometry
# with no mid portal), which is the only honest way to place a portal by tick:
#
#   ufo    +333.13   dy +0.53 against a free +1.44   (both ufo units agree
#                                                     to 0.19 px)
#   cube   +279.02   dy +1.28 against a free +3.38
#   ball   +301.47   dy +0.37 against a free +3.28
#
# Every one is CLAMPED SHORT on the contact tick -- the body would have gone
# 1 to 3 px further into the ramp and GD stops it at the seat. That is the
# arrival lv20's second body has (its own tick moves -2.583 against a velocity
# of +2.86) and the one `ridemode` never produced: there the body slides on and
# the first tick moves -0.354, LESS than a full ride step and in the same
# direction it was already going.
RIDEDROP_CONTACT = {"ufo": 333.13, "cube": 279.02, "ball": 301.47}


def build_ridedrop() -> str:
    u"""Does contact ACQUIRED BY A CLAMP start a ride? lv20's last difference.

    Pass 1 is this same file with `RIDEDROP_CONTACT` empty: build the drop
    geometry, let the run say where each mode meets the ramp, and read it back
    with `grabcontact`. Pass 2 places the mid portal 0, 1 and 2 ticks after that
    contact -- lv20's is 1 -- so the answer does not hang on a single placement
    being right to the tick.

    The `wave` unit is LAST and dies: it is lv20's own target, and one death
    ends the run.
    """
    objs: list[str] = []
    x = 90.0
    plan = [(mode, target, k)
            for mode, target in (("ufo", "ball"), ("cube", "ball"),
                                 ("ball", "cube"))
            for k in (0, 1, 2)]
    plan.append(("ufo", "wave", 1))
    for mode, target, k in plan:
        x0 = x
        port = None
        if mode in RIDEDROP_CONTACT:
            port = x0 + RIDEDROP_CONTACT[mode] + k * DX_1X + 32.0
        u, x, info = ridedrop_unit(x, mode, target, False, port)
        objs += u
        UNITS.append({"x0": x0, "x1": x, "mode": mode, "target": target,
                      "m": 1.0, "mini": 0, "ceiling": 1, "age": k, **info})
    objs += floor_run(0, PAVE_X)
    return header() + ";" + ";".join(objs) + ";"


# THE CEILING HAS TO SIT UNDER THE BAND, and 380 does not. `ceil_unit_dual`
# picked DUAL_CEIL = 380 to clear the deepest |m|=2 chain; measured on the first
# cut of this rig, a `header(dual=True)` level reports pmax = 360, so the second
# body is held by the INVISIBLE CEILING at 345 and never reaches a ceiling laid
# at 380 -- it ran the whole level as a cube, and not one head portal fired.
# 330 leaves the band alone: the body hangs at 315 and the three |m|=1 ramps
# bottom out at 240.
RIDEDUAL_CEIL = 330.0


def ridemodedual_unit(x: float, mode: str, target: str, m: float, mini: bool
                      ) -> tuple[list[str], float, dict]:
    u"""`ridemode`, but THE SECOND BODY of a dual is the one on the ramp.

    `ceil_unit_dual` with a mode portal added mid-ride, and it inherits that
    unit's three rules unchanged, all of them forced by the pair: no gravity
    portals (the second body is already inverted and has to stay that way from
    one unit to the next), the head portals AT THE CEILING (a floor-height
    portal is taken by the first body and never reached by the second --
    measured on `dualmode`, 3,471 consecutive ticks with the halves in different
    modes), and ONE ceiling height for every unit, since the second body flies
    between them.

    Why this rig exists: three cuts of `ridemode` have now failed to reproduce
    lv20 t=17,117. GD takes the ride's own step through a mode portal on a floor
    ramp and on a ceiling one, in every mode, at both sizes, with the box the
    same size or shrinking to a fifth, and with the ride anything from ONE tick
    to forty ticks old -- 29 of 29, and then 1 of 1 at age 0. lv20's body does
    none of that: it steps +0.029, the free step, and takes the ramp exit's
    -2.762 on the portal tick itself.

    Everything that rig can vary has been varied. What is left is the thing it
    cannot: THE BODY IS THE SECOND HALF OF A DUAL. The first body agrees with
    the model tick for tick through the whole site, so if being the second half
    is what does it, that is the last place it can be hiding.
    """
    oid, rot, fx, fy, w, h = CEIL_RAMP[m]
    objs: list[str] = []
    ceil = RIDEDUAL_CEIL
    objs.append(obj(MODE_PORTAL[mode], x, ceil - 15.0))
    objs.append(obj(SIZE_MINI if mini else SIZE_NORM, x + 2 * GRID, ceil - 15.0))
    n_ramps = 3
    x_ramp = x + 20 * GRID
    # NO PER-UNIT FLAT CEILING: `build_ridemodedual` lays one continuous run
    # from x=0, because the second body is launched upward at the level's start
    # and clears 435 before the first unit's ceiling would have begun -- measured
    # on cut 2 of this rig, where it then sat above the ceiling for the whole
    # level and not one mid-ramp portal fired.
    for i in range(n_ramps):
        cy = ceil - h / 2.0 - i * h
        objs.append(obj(oid, x_ramp + i * w + w / 2.0, cy,
                        rot=rot, flip_x=fx, flip_y=fy))
        yy = cy + h / 2.0 + GRID / 2
        while yy <= ceil + GRID / 2 + 1.0:
            for k in range(int(w / GRID)):
                objs.append(obj(BLOCK, x_ramp + i * w + GRID / 2 + k * GRID, yy))
            yy += GRID
    port_cx = x_ramp + 2.5 * w
    travel = max(0.0, port_cx - 32.0 + 6.2 - x_ramp)
    i_at = min(n_ramps - 1, int(travel / w))
    surf = ceil - i_at * h - (travel - i_at * w) * (h / w)
    port_cy = surf - (9.0 if mini else 15.0)
    objs.append(obj(MODE_PORTAL[target], port_cx, port_cy))
    x_flat = x_ramp + n_ramps * w
    x_end = x_flat + 16 * GRID
    return objs, x_end + 14 * GRID, {"x_ramp": x_ramp, "ramp_w": w,
                                     "ramp_h": h, "port_cx": port_cx,
                                     "port_cy": port_cy, "ceil_y": ceil}


def build_ridemodedual() -> str:
    u"""The second body of a dual meets a mode portal mid-ride. lv20's own shape.

    The `wave` unit is LAST and cannot survive: a flipped wave with no input
    flies straight back into the ceiling it just left. A p2 death ends the run
    ("dual: booked a p2 death as the run end"), so everything after it would be
    lost -- and it is the one unit that matters most, so it also gets the
    cheapest possible approach to it.
    """
    objs: list[str] = []
    x = 90.0
    m = 1.0
    for mode, target in (("ufo", "ball"), ("ball", "cube"), ("ufo", "spider"),
                         ("ufo", "wave")):
        x0 = x
        u, x, info = ridemodedual_unit(x, mode, target, m, False)
        objs += u
        UNITS.append({"x0": x0, "x1": x, "mode": mode, "target": target,
                      "m": m, "mini": 0, "ceiling": 1, "dual": 1, **info})
    # One ceiling for the whole level, laid last so its uid is above the ramps'
    # (build_ramps L305). The ramps hang BELOW it, so where they exist they are
    # the surface the second body meets and this row is inert.
    xx = GRID / 2
    while xx <= x + 20 * GRID:
        objs.append(obj(BLOCK, xx, RIDEDUAL_CEIL + GRID / 2))
        xx += GRID
    objs += floor_run(0, PAVE_X)
    return header(dual=True) + ";" + ";".join(objs) + ";"


def build_ceilhold() -> str:
    u"""Rig that HOLDS THE BUTTON DOWN while riding a ceiling ramp (the pressed
    version of ceilramp).

    The corpus' last ceiling family, lv18@3,010 (mini ship, m=-1, in=1), has the
    shape "GD writes vy=-2.000 ONLY ON THE TICK THE BUTTON IS PRESSED WHILE
    RIDING THE HANGING SIDE" (y matches bit for bit on every tick; the only
    thing that splits is vy, on a single tick):

        t=3,007..3,009 in=0  GD vy=0.000 (model 0.000 too)
        t=3,010        in=1  GD vy=-2.000 / model -0.101
        t=3,011        in=0  GD vy=0.000 (model 0.000 too)

    The floor side got the same rule in r39a (`c.mode==1 && input` -> 2.000*dir
    on attachment, then +kShipRampG*dir). THE HANGING-SIDE BRANCH (flipped,
    sliding along the underside of a ceiling ramp) DOES NOT HAVE IT -- that side
    only has the `if vy>0 then 0` whitelist. The r39 note saying "the downhill
    (dir=-1) ladder is unmeasured" was about exactly this.

    Same geometry as ceilramp (which passes 43/43) with a single press window
    added. The window is 35%-75% of the ramp chain -- the middle, which fouls
    neither the attachment nor the launch -- and lasts 10-30 ticks, so a single
    run settles "is it written once or every tick" and "does the ladder build
    up" as well.

    MODE ORDER: modes that leave the surface when pressed (wave takes GD down on
    its own units of ceilramp; spider warps to the opposite surface) GO LAST
    (the rig's rule).
    """
    objs: list[str] = []
    x = 90.0
    # [2026-08-21 measured] WAVE GOES LAST. It took GD itself down at unit36
    # (wave's first), leaving all 6 spider units EMPTY (same as ceilramp).
    for mode in ("ship", "ufo", "cube", "ball", "robot", "swing",
                 "spider", "wave"):
        for m in (1.0, 0.5, 2.0):
            for mini in (False, True):
                x0 = x
                u, x = ceil_unit(x0, mode, m, mini)
                objs += u
                w, h = CEIL_RAMP[m][4], CEIL_RAMP[m][5]
                x_ramp = x0 + 20 * GRID
                chain = 3 * w
                xp0 = x_ramp + 0.35 * chain
                xp1 = x_ramp + 0.75 * chain
                t_press, t_rel = tick_at(xp0), tick_at(xp1)
                PLAN.append((t_press, 1))
                PLAN.append((t_rel, 0))
                UNITS.append({"x0": x0, "x1": x, "mode": mode, "m": m,
                              "mini": int(mini),
                              "x_ramp": x_ramp, "ramp_w": w, "ramps": 3,
                              "ceil_y": ceil_y_for(h, 3),
                              "hold_x0": round(xp0, 2),
                              "hold_x1": round(xp1, 2),
                              "t_press": t_press, "t_rel": t_rel})
    objs += floor_run(0, PAVE_X)
    return header() + ";" + ";".join(objs) + ";"


def build_portrot() -> str:
    u"""Rig for measuring THE HITBOX OF A ROTATED PORTAL. Zero input.

    On lv21 uid24194 (id111 UFO portal, rot=43, raw size 34x86, bounding box
    167x172) GD fires at a point 54.07px in x from the centre. That is neither
    the bounding box's 83.5 nor the ~41.7 of "a 34x86 box rotated by 43
    degrees". Sweep the angle to recover the shape of the half-width.

    One unit = [cube portal (reset, rot 0)][run-up][UFO portal (rot theta)].
    The player runs as a normal-size cube, and the x of the tick where mode
    changes to ufo gives the effective half-width
    = portal_cx - x_fire + 15. With no input a UFO just falls and slides along
    the ground, so the next unit's cube portal can reset it.
    """
    objs: list[str] = []
    x = 90.0
    objs += floor_run(0, PAVE_X)
    for rot in (0, 10, 20, 30, 40, 43, 45, 50, 60, 70, 80, 90):
        x0 = x
        objs.append(obj(P_CUBE, x, GROUND_TOP + 15.0))
        x_port = x + 10 * GRID
        objs.append(obj(P_UFO, x_port, GROUND_TOP + 15.0, rot=float(rot)))
        x = x_port + 20 * GRID
        UNITS.append({"x0": x0, "x1": x, "rot": rot, "x_port": x_port,
                      "mode": "ufo", "m": 0, "mini": 0, "player": "cube"})
    return header() + ";" + ";".join(objs) + ";"


def build_portwave() -> str:
    u"""The portrot rig with THE PLAYER AS A MINI WAVE. Zero input.

    Working back from lv21 t=18,087 says "a mini wave's portal contact half-size
    is 15.9-16.8", yet the model's kWaveContactHalfMini is 3.0 (fitted to the
    lv20 symptom). SINCE EVERY PORTAL ON lv20 IS rot=0 AND lv21's IS 43 DEGREES,
    measuring both the axis-aligned case (the boundary is a + w0/2, so the x
    half-size a acts directly) and the rotated case (what binds is the box's u
    axis, a mix of a and the y half-size b) separates a from b.

    With no input a wave travels diagonally downward, so it slides along the
    ground. The portals are placed at a height that overlaps the player's path
    (just above the ground).
    """
    objs: list[str] = []
    x = 90.0
    objs += floor_run(0, PAVE_X)
    for rot in (0, 20, 43, 60, 90):
        x0 = x
        # Reset to cube every time, then go mini wave (the size is rebuilt too)
        objs.append(obj(P_CUBE, x, GROUND_TOP + 15.0))
        objs.append(obj(SIZE_MINI, x + 2 * GRID, GROUND_TOP + 15.0))
        objs.append(obj(P_WAVE, x + 4 * GRID, GROUND_TOP + 15.0))
        x_port = x + 14 * GRID
        objs.append(obj(P_UFO, x_port, GROUND_TOP + 15.0, rot=float(rot)))
        # Back to normal size for the next unit
        objs.append(obj(SIZE_NORM, x_port + 10 * GRID, GROUND_TOP + 15.0))
        x = x_port + 24 * GRID
        UNITS.append({"x0": x0, "x1": x, "rot": rot, "x_port": x_port,
                      "mode": "ufo", "m": 0, "mini": 1, "player": "wave"})
    return header() + ";" + ";".join(objs) + ";"


def build_rampjump() -> str:
    u"""Rig for measuring the bonus added when JUMPING WHILE STILL ON A RAMP.

    `ramps` is a zero-input rig measuring the launch on "leaving the top of the
    ramp", so it never yields the value for a press INSIDE the ramp window. GD
    adds the ride contribution to the plain jump there (updateJump 0x38bc77,
    gated on this+0xb58). The 22 official levels only sample this 3 times, all
    at |m|=0.5, so the gradient dependence cannot be separated.

    The shape is the same as ramp_unit, with one press placed PARTWAY through
    the n ramps.
    - The press x is at 70% of the ramp band (well into the ride, but before the
      top end)
    - n=3 exceeds a 24-tick ride, so the ramp factor saturates at 1.0
    - cube and robot only (a ball's tap flips gravity, a spider teleports)
    - Do not hold (press -> release 3 ticks later), so a held re-jump does not
      leak into the next unit

    BUILD IT IN TWO PASSES: run it once without --xmap, take the dump, and
    regenerate passing that dump to --xmap (the x prediction of the press tick
    drifts on a long rig).
    """
    objs: list[str] = []
    x = 90.0
    objs += floor_run(0, PAVE_X)
    for mode in ("cube", "robot"):
        for m in (1.0, 0.5, 2.0):
            for mini in (False, True):
                x0 = x
                _oid, _rot, _fx, _fy, w, _h = RAMP[m]
                # Breakdown of ramp_unit: 2 portals -> 8 cells of run-up -> 3 ramps
                x_ramp0 = x + 8 * GRID
                x_press = x_ramp0 + 3 * w * 0.70
                u, x = ramp_unit(x, mode, m, mini, 3, 0.0)
                objs += u
                t = tick_at(x_press)
                PLAN.append((t, 1))
                PLAN.append((t + 3, 0))
                UNITS.append({"x0": x0, "x1": x, "mode": mode, "m": m,
                              "mini": int(mini), "ramps": 3, "bury": 0.0,
                              "press_x": x_press, "press_t": t})
    return header() + ";" + ";".join(objs) + ";"


def build_orbs() -> str:
    """Orb calibration rig. Ground modes x orb kinds x normal/mini.

    On the 22 official levels, orbs are the family that mass-produces
    contact-boundary (+/-1 tick) fixups (12 orbnear on lv10, the kills on
    lv19/lv21, etc.), and the per-mode ratios only ever existed as "measured
    individually". This measures them all at once.
    """
    objs: list[str] = []
    x = 90.0
    for mode in ("cube", "ball", "robot", "spider"):
        for kind in ("yellow", "pink", "blue", "green", "red", "black",
                     "dash", "gravdash", "spider"):
            for mini in (False, True):
                x0 = x
                u, pl, x, tp, tr = orb_unit(x, kind, mode, mini, 60.0)
                objs += u
                PLAN.extend(pl)
                # PUT THE PRESS WINDOW IN THE TABLE. If the extractor takes "the
                # first spike inside the unit", it misreads the landing of the
                # previous unit's arc as a fire (measured: every mini unit was
                # filled with the previous unit's landing). Look only inside the
                # window.
                UNITS.append({"x0": x0, "x1": x, "mode": mode, "orb": kind,
                              "mini": int(mini), "t_press": tp, "t_rel": tr})
    return header() + ";" + ";".join(objs) + ";"


def orbair_unit(x: float, kind: str, mode: str, mini: bool, off: float,
                early: bool = False
                ) -> tuple[list[str], list[tuple[int, int]], float, int, int]:
    """An orb unit that MAKES THE TAP HAPPEN IN MID-AIR (a curtain of orbs).

    On the ground-tap rig, robot/spider produced the mode's own action
    (5.590 / 1.000+flip) and the orb's value could not be obtained. In mid-air
    there is no grounded action, so the press can only mean the orb firing.

    The launch uses the ALREADY-MEASURED slope-exit launch (|m|=1, saturated
    with 3 ramps). The arc is only about 30px tall, so stacking the same orb
    vertically into a "curtain" guarantees that any trajectory touches one of
    them -- no need to predict the arc (the launch value and the gravity both
    differ per mode, so predicting would make the rig mode-dependent).
    `off` is the x offset from the top end; sweeping it sweeps THE INCOMING vy
    (small = still rising, large = already falling).
    """
    oid, _ = ORB[kind]
    ramp_id, rot, fx, fy, w, h = RAMP[1.0]
    objs: list[str] = []
    y0 = GROUND_TOP + 15.0
    objs.append(obj(MODE_PORTAL[mode], x, y0))
    objs.append(obj(SIZE_MINI if mini else SIZE_NORM, x + 2 * GRID, y0))
    # 3 ramps climb from world 90 to 180, with the launch at the top end
    xr = x + 8 * GRID
    bottom = GROUND_TOP
    for i in range(3):
        objs.append(obj(ramp_id, xr + w / 2.0, bottom + h / 2.0,
                        rot=rot, flip_x=fx, flip_y=fy))
        yy = GROUND_TOP + GRID / 2
        while yy < bottom:
            objs.append(obj(BLOCK, xr + GRID / 2, yy))
            yy += GRID
        bottom += h
        xr += w
    x_top = xr                      # the launch is on the tick that leaves here
    # The curtain: a vertical column covering the arc's height (90px above and
    # below the top end)
    x_orb = x_top + off
    yy = bottom - 60.0
    while yy <= bottom + 90.0:
        objs.append(obj(oid, x_orb, yy))
        yy += GRID
    # The press window opens WELL AFTER THE LAUNCH. At +12px the launch tick
    # falls inside the window for some modes, and the extractor misread the
    # launch (incoming vy=0 -> 7.405) as the orb (every robot/spider unit was
    # filled with that). The player's half-width differs per mode, so the x at
    # which it leaves the top end differs too -- leaving margin is the right
    # answer.
    # [2026-08-22] `early`: arrive WITH THE BUTTON HELD FROM LONG BEFORE. In the
    # corpus, lv21@7,217 touches the black orb after 117 TICKS OF HOLDING (GD's
    # `orb:` line, press=7098 lag=117), and only there does the terminal clamp
    # come 1 tick late. The rig's default (45px past the launch = a lag of a few
    # ticks) shows no such delay, so this sweeps whether the press length is the
    # discriminator.
    # early starts pressing RIGHT AFTER THE LAUNCH (in mid-air). Pressing on the
    # ground consumes the press on the player's own jump and the orb never fires
    # (the v3 failure: all 8 units `no drop`). Start at +2px, which does not
    # straddle the launch tick.
    t_press = tick_at(x_top + 2.0) if early else tick_at(x_top + 45.0)
    t_rel = tick_at(x_orb + 24.0)
    x_next = x_orb + 40 * GRID
    # A ceiling for the kinds that flip gravity (blue/green/gravity dash), plus
    # a gravity reset at the end of the unit. Without them the player "falls"
    # upward and dies off-screen (the same trap as the orb rig).
    ceil = GROUND_TOP + 330.0
    xx = x - GRID
    while xx <= x_next:
        objs.append(obj(BLOCK, xx, ceil + GRID / 2))
        xx += GRID
    objs.append(obj(GRAV_NORM, x_next - 4 * GRID, y0))
    objs.append(obj(GRAV_NORM, x_next - 4 * GRID, ceil - 15.0))
    return objs, [(t_press, 1), (t_rel, 0)], x_next, t_press, t_rel


def build_orbair() -> str:
    """Mid-air-tap orb rig. 4 orb kinds x 4 ground modes x 4 incoming points
    (normal size).

    Two things are wanted: (1) can the orb's value be obtained on robot/spider,
    (2) DOES THE FIRE VALUE DEPEND ON THE INCOMING vy (if it does not, it is an
    assignment, and the orbnear fixups are a problem of the contact tick rather
    than of the value).
    """
    objs: list[str] = []
    x = 90.0
    for mode in ("cube", "ball", "robot", "spider"):
        for kind in ("yellow", "pink", "blue", "red"):
            for off in (60.0, 90.0, 120.0, 150.0):
                x0 = x
                u, pl, x, tp, tr = orbair_unit(x, kind, mode, False, off)
                objs += u
                PLAN.extend(pl)
                UNITS.append({"x0": x0, "x1": x, "mode": mode, "orb": kind,
                              "mini": 0, "off": off,
                              "t_press": tp, "t_rel": tr})
    return header() + ";" + ";".join(objs) + ";"


def dualport_unit(x: float, kind: str, mode: str
                  ) -> tuple[list[str], float, dict]:
    u"""One portal unit inside a dual. ZERO INPUT.

    The corpus' lv16@13,496 is "a same-mode ship portal and a normal-gravity
    portal take effect on the same tick, GD applies x1/4, and upsideDown does
    not move", and that single sample admits at least three readings (2 ship
    fly-ends + gravity passing through / 1 ship fly-end + gravity halving
    without a flip / ...). MEASURING THE SITUATIONS ONE AT A TIME is what this
    rig is for.

    kind:
      same   only a same-mode mode portal (ship->ship)
      gnorm  only a normal-gravity portal (id10)
      ginv   only an inverting gravity portal (id11)
      both   same + gnorm overlaid (reproduces lv16)
      othr   only a different-mode portal (ship->UFO) = the control for two fly-ends
      size   only a size portal (a control that does not halve)

    The portals are STRETCHED VERTICALLY WITH scale SO THEY HIT BOTH PLAYERS
    (a dual's two players stick to the top and bottom of the band, so a raw
    34x86 only hits one of them).
    """
    objs: list[str] = []
    # The track is A NARROW 60px CORRIDOR. A dual's two players stick to the top
    # and bottom of the band, so without a narrow corridor the lv16 situation of
    # "one portal hitting both players" cannot be created. A raw portal (34x86)
    # centred in the corridor covers both.
    # Stretching with scale STRETCHES IT HORIZONTALLY TOO AND OVERLAPS THE
    # NEIGHBOURING UNIT (the v1 failure: scale 4 made it 136px wide and both the
    # mode and the size leaked in from the previous unit).
    #
    # [v3] WIDEN THE CORRIDOR JUST BEFORE THE PORTAL. In v2 both players passed
    # through at rest on a surface with vy=0, so THE HALVING WAS IN PRINCIPLE
    # INVISIBLE (the coefficient on vy is exactly what we want). Raising the
    # floor to 200 and the ceiling to 260, then opening the floor to 170 and the
    # ceiling to 290 25px before the portal, gives both players 30px of free
    # flight so they enter the portal at |vy|~2.
    # [v4] The floor STAYS AS THE WORLD GROUND and the ceiling is fixed 150px up
    # (240).
    #   v3  = stacked a floor -> the spawn position was inside a solid and the
    #         whole rig died instantly
    #   v3b = opened the ceiling partway -> the two players never returned to the
    #         corridor, and later units' mode/size portals only hit one of them
    # v4 LAUNCHES THE LOWER PLAYER (p1) WITH A YELLOW PAD and lets it enter the
    # portal in flight. The corridor height never changes, so the units stay
    # independent. The upper player (p2) stays at rest on the ceiling = "what
    # happens to the partner that is NOT inside the portal's box" can be read at
    # the same time (a test of the dual mirror rule).
    # [v5] When kind is "g2in", shape it so BOTH PLAYERS ARE INSIDE THE SAME BOX
    # (reproducing lv16@13,496). Lowering the ceiling to 200 puts the upper
    # player at rest at 185, only 10px from the apex (105+70=175) of the lower
    # player launched by the yellow pad. Placing the portal at the apex's x
    # (97px past the pad) covers both with a single portal.
    # [v6 failure] Lowering the ceiling to 200 for g2in crushed the yellow pad's
    # launch (vy~8.9) against the ceiling and killed instantly. KEEP THE
    # CORRIDOR HEIGHT THE SAME FOR EVERY UNIT and instead PLACE THE PORTAL
    # HIGHER (cy=200): p1 near its apex (175) and p2 on the ceiling (225) are
    # then both inside one box (157..243).
    two_in = (kind == "g2in")
    ceil = GROUND_TOP + 150.0                          # 240
    x_end = x + 40 * GRID
    xp = x + 20 * GRID
    xx = x - GRID
    while xx <= x_end:
        objs.append(obj(BLOCK, xx, ceil + GRID / 2))
        xx += GRID
    # The launch: normally 45px before (entering on the rising part of the arc),
    # while g2in enters at the apex
    objs.append(obj(PAD_YELLOW, xp - (97.0 if two_in else 45.0),
                    GROUND_TOP + 5.0))
    cy = GROUND_TOP + (110.0 if two_in else 55.0)      # 200 / 145
    # Re-apply mode/size at the head of every unit (to avoid carry-over from the
    # previous unit). PLACE THEM AT A HEIGHT THAT HITS BOTH PLAYERS (at floor
    # height they only hit p1).
    objs.append(obj(MODE_PORTAL[mode], x, cy))
    objs.append(obj(SIZE_NORM, x + 3 * GRID, cy))
    if kind in ("same", "both"):
        objs.append(obj(MODE_PORTAL[mode], xp, cy))
    if kind == "othr":
        objs.append(obj(P_UFO if mode != "ufo" else P_SHIP, xp, cy))
    # [both] Reproducing lv16 means OVERLAYING THEM AT THE SAME x (so the mode
    # portal and the gravity portal take effect on the same tick). Separating
    # them would only measure "whichever fired first".
    if kind in ("gnorm", "both", "g2in"):
        objs.append(obj(GRAV_NORM, xp, cy))
    if kind == "ginv":
        objs.append(obj(GRAV_FLIP, xp, cy))
    if kind == "size":
        objs.append(obj(SIZE_MINI, xp, cy))
    u = {"x0": x, "x1": x_end, "mode": mode, "kind": kind,
         "x_port": xp, "port_cy": cy}
    return objs, x_end, u


def build_dualport() -> str:
    u"""Rig for portals inside a dual. DUAL FROM THE START, VIA THE HEADER (kA8=1).

    With zero input p1 sticks to the floor and p2 to the ceiling, both sliding.
    A vertically stretched portal is then passed over them, and HOW THE vy AND
    upsideDown OF BOTH PLAYERS CHANGE is read from the dump's p2y/p2vy/p2up
    columns.
    """
    objs: list[str] = []
    x = 90.0
    # g2in (both players inside the same box = reproducing lv16@13,496) has a
    # ceiling 40px lower, so PUT IT FIRST. Placed later, the player stuck to the
    # ceiling ends up inside a solid at the boundary where the ceiling drops and
    # the whole rig dies (the v5 failure: A STEP CAN ONLY BE BUILT UPWARD).
    # [UNFINISHED - NEXT MOVE] g2in still has no working shape. The three
    # attempts and their results:
    #   lower the ceiling to 200 -> the yellow pad (vy~8.9) is crushed against
    #                               the ceiling, instant death
    #   put g2in last            -> at the boundary where the ceiling drops, the
    #                               upper player enters a solid and dies
    #   move only the portal to 200 -> the first one passes, but on the leading
    #                               unit THE PLAYER STRADDLES AND OVERSHOOTS THE
    #                               MODE PORTAL AT THE HEAD (mode stays cube)
    # What is needed is "both players inside an 86px box at once, with at least
    # one of them in flight". The next ideas are a weaker pad (pink), or putting
    # the lower player on a platform to close the distance.
    for mode in ("ship", "ufo"):
        for kind in ("same", "gnorm", "ginv", "both", "othr", "size"):
            u_objs, x_next, u = dualport_unit(x, kind, mode)
            objs += u_objs
            UNITS.append(u)
            x = x_next
    objs += floor_run(0, x + 600.0)
    return header(dual=True) + ";" + ";".join(objs) + ";"


def build_dropair() -> str:
    u"""Rig for STEPPING ON A BLACK ORB (drop ring) IN MID-AIR. The black-orb
    version of orbair.

    The corpus' lv21@7,217 shows a 1-tick hole: "on the tick AFTER exactly
    -15.000 (the terminal velocity) is assigned, GD emits -15.216 (= terminal +
    one gravity step), and the tick after that it is back to -15.000" (the only
    such case across all 22 gdref levels). On the ground-based orbs rig the
    player lands on the very next tick and it cannot be seen, so a shape where
    IT IS STEPPED ON IN MID-AIR WITH ROOM TO FALL BELOW is required.

    The black orb assigns its value ignoring the incoming vy (cube/ball/robot
    15.0 / spider 16.5 / ship/wave/swing 14.0 / UFO 11.2), so the entry point
    (off) is only there to confirm "the fire really was the orb". What is read
    is ONLY THE FIRE TICK AND THE 3 TICKS AFTER IT.
    """
    objs: list[str] = []
    x = 90.0
    # [v2] SWEEP THE INCOMING vy. v1's 4 modes x 2 points could only produce a
    # counterexample (ball) to "stepping on it while rising delays it by 1
    # tick". Sweeping off (the x offset from the top of the arc) finely moves
    # the entry from rising to apex to falling, so where in the incoming vy the
    # delay boundary sits can be read off. cube and ball are laid out side by
    # side (the delayed sample in the corpus is cube, the rig's counterexample
    # is ball).
    for mode in ("cube", "ball"):
        for off in (20.0, 30.0, 40.0, 50.0, 70.0, 90.0):
            x0 = x
            u, pl, x, tp, tr = orbair_unit(x, "black", mode, False, off)
            objs += u
            PLAN.extend(pl)
            UNITS.append({"x0": x0, "x1": x, "mode": mode, "orb": "black",
                          "mini": 0, "off": off,
                          "t_press": tp, "t_rel": tr})
    # The version that ARRIVES WITH THE BUTTON HELD (a test of whether lag is
    # the discriminator; see the note above).
    # Pressing early (starting in mid-air) fires immediately on the rising part
    # of the arc, so OFF BECOMES THE KNOB THAT SWEEPS THE INCOMING vy. This
    # brackets the delay boundary (for cube it does not appear at +1.337 but
    # does at +2.221).
    for mode in ("cube", "ball"):
        for off in (35.0, 45.0, 55.0, 60.0, 61.0, 62.0, 63.0, 64.0, 65.0,
                    75.0, 85.0):
            x0 = x
            u, pl, x, tp, tr = orbair_unit(x, "black", mode, False, off,
                                           early=True)
            objs += u
            PLAN.extend(pl)
            UNITS.append({"x0": x0, "x1": x, "mode": mode, "orb": "black",
                          "mini": 0, "off": off, "early": 1,
                          "t_press": tp, "t_rel": tr})
    for mode in ("cube", "ball", "robot", "spider"):
        for off in (60.0, 90.0, 120.0):
            x0 = x
            u, pl, x, tp, tr = orbair_unit(x, "black", mode, False, off,
                                           early=True)
            objs += u
            PLAN.extend(pl)
            UNITS.append({"x0": x0, "x1": x, "mode": mode, "orb": "black",
                          "mini": 0, "off": off, "early": 1,
                          "t_press": tp, "t_rel": tr})
    for mode in ("cube", "ball", "robot", "spider"):
        for mini in (False, True):
            for off in (60.0, 120.0):
                x0 = x
                u, pl, x, tp, tr = orbair_unit(x, "black", mode, mini, off)
                objs += u
                PLAN.extend(pl)
                UNITS.append({"x0": x0, "x1": x, "mode": mode, "orb": "black",
                              "mini": int(mini), "off": off,
                              "t_press": tp, "t_rel": tr})
    # Flight modes go last (how they enter the slope launch is mode-dependent,
    # and if one dies everything after it comes out EMPTY -- the rig's rule)
    for mode in ("ship", "ufo"):
        for off in (60.0, 120.0):
            x0 = x
            u, pl, x, tp, tr = orbair_unit(x, "black", mode, False, off)
            objs += u
            PLAN.extend(pl)
            UNITS.append({"x0": x0, "x1": x, "mode": mode, "orb": "black",
                          "mini": 0, "off": off, "t_press": tp, "t_rel": tr})
    return header() + ";" + ";".join(objs) + ";"


def orbedge_unit(x: float, kind: str, mode: str, mini: bool, delta: float,
                 dy: float = 0.0
                 ) -> tuple[list[str], list[tuple[int, int]], float, int, int]:
    """A unit for sweeping the fire boundary. PRESS FOR EXACTLY 1 TICK.

    What is wanted is "the x boundary at which the orb fires". A tick step is
    1.298px, so sweeping the press tick caps the resolution there -- instead,
    PIN THE PRESS TICK AND MOVE THE ORB 1px AT A TIME. The switch between fire
    and no-fire is the boundary, at a resolution of 1px (in fact at whatever
    precision the generator chooses).

    `delta` is the orb centre's offset from "the player's x at the press tick".
    The player's real x is read from the dump, so any prediction error is
    absorbed on the extraction side.
    """
    oid, _ = ORB[kind]
    objs: list[str] = []
    y = GROUND_TOP + 15.0
    objs.append(obj(MODE_PORTAL[mode], x, y))
    objs.append(obj(SIZE_MINI if mini else SIZE_NORM, x + 2 * GRID, y))
    x_ref = x + 8 * GRID          # press for exactly 1 tick here
    x_orb = x_ref + delta
    objs.append(obj(oid, x_orb, y + dy))
    t = tick_at(x_ref)
    # A 1-tick press: if the boxes overlap on that tick it fires, otherwise the
    # player's own action happens (a jump, for a grounded cube). Which one
    # occurred is told by the value of vy.
    # x_orb is returned so the extraction side can compute the gap from "the
    # player's x at the press tick" FROM THE MEASUREMENT (using delta directly
    # as the gap adds tick_at's 1.3px rounding).
    return objs, [(t, 1), (t + 1, 0)], x_ref + 20 * GRID, t, x_orb


def build_orbedge() -> str:
    """Sweep rig for the orb fire boundary (PINK orb, cube normal/mini, -40..+40px).

    An orb is reported as 36x36, so with a plain AABB the boundary would be
    18+15 = 33px for normal and 18+9 = 27px for mini. ANY DEVIATION FROM THAT
    MEANS THE TEST IS NOT RECTANGULAR (a radius test, or an inner box), which
    bears directly on the body of the orbnear fixup family (already established
    to be a problem of the contact tick, not of the value).

    USE PINK, NOT YELLOW. A grounded cube's 1-tick press becomes a jump (11.180)
    when the orb is out of reach, which is the same value as the yellow orb and
    therefore indistinguishable. Pink is 8.050, so whether it fired can be read
    uniquely from the value of vy.
    """
    objs: list[str] = []
    x = 90.0
    for mini in (False, True):
        for d in range(-40, 41):
            x0 = x
            u, pl, x, tp, x_orb = orbedge_unit(x, "pink", "cube", mini,
                                               float(d))
            objs += u
            PLAN.extend(pl)
            UNITS.append({"x0": x0, "x1": x, "mode": "cube", "orb": "pink",
                          "mini": int(mini), "delta": d,
                          "t_press": tp, "t_rel": tp, "x_orb": x_orb})
    return header() + ";" + ";".join(objs) + ";"


def build_orbedge2() -> str:
    """A TWO-DIMENSIONAL sweep of the fire region (pink, normal cube). Settles the
    shape of the test.

    The 1-D sweep at dy=0 put the boundary at a clamped distance of 18.0-18.13,
    which does not lie on the same circle as the "clamped 22.996 fires" from
    lv12 noted in the model. So step dy and sweep dx finely to produce THE
    BOUNDARY CURVE dx_max(dy).

    Distinguishing the predictions:
      circle vs box (the model's current shape, radius R):
        dx_max = 15 + sqrt(R^2 - (dy-15)^2)
        -> once dy passes 15, dx_max shrinks and drops to 15 at dy = 15+R
      plain AABB: dx_max stays at 33 regardless of dy, and everything above
        |dy| > 33 fails to fire at once
    One run tells the two apart.
    """
    objs: list[str] = []
    x = 90.0
    for dy in (0.0, 10.0, 20.0, 25.0, 30.0, 32.0, 34.0):
        for d in range(10, 41, 2):
            x0 = x
            u, pl, x, tp, x_orb = orbedge_unit(x, "pink", "cube", False,
                                               float(d), dy)
            objs += u
            PLAN.extend(pl)
            UNITS.append({"x0": x0, "x1": x, "mode": "cube", "orb": "pink",
                          "mini": 0, "delta": d, "dy": dy,
                          "t_press": tp, "t_rel": tp, "x_orb": x_orb})
    return header() + ";" + ";".join(objs) + ";"


def build_orbedge3() -> str:
    """A MODE SWEEP of the fire boundary (uses the spider to settle how the
    half-width is treated).

    For a normal cube the boundary came out as 33 = orb half 18 + player half
    15, but it is not yet decided whether that is
      (a) orb + THE HALF-WIDTH OF THAT MODE  -> 31.5 for a spider (13.5)
      (b) orb + ALWAYS 15                    -> 33 even for a spider
    lv22's rotating section goes through spider/ball, so the AABB cannot be
    installed until this is settled (reach_check blocked it).

    A spider's own action is a teleport (vy +/-1.000), so it is distinguishable
    from a pink fire by value. Both normal and mini, to be safe.
    """
    objs: list[str] = []
    x = 90.0
    for mode, mini, own in (("spider", False, 1.000), ("spider", True, 1.000),
                            ("robot", False, 5.590), ("robot", True, 4.472)):
        for d in range(24, 37):
            x0 = x
            u, pl, x, tp, x_orb = orbedge_unit(x, "pink", mode, mini,
                                               float(d), 0.0)
            objs += u
            PLAN.extend(pl)
            UNITS.append({"x0": x0, "x1": x, "mode": mode, "orb": "pink",
                          "mini": int(mini), "delta": d, "own": own,
                          "t_press": tp, "t_rel": tp, "x_orb": x_orb})
    return header() + ";" + ";".join(objs) + ";"


def crush_unit(x: float, gap: float, mode: str, mini: bool) -> tuple[list[str], float, dict]:
    u"""One crush corridor: a floor pillar (top 210) + a ceiling pillar
    (underside 210+gap).

    The player runs on the real ground (world 90), and the pillars hang high
    enough that it can pass underneath the corridor. An injection (inject y=225)
    seats it on the floor, so we can measure what GD does when the gap is
    smaller than the player's height (crush / push out / pass through).
    The rig is static, so no Move trigger is needed.
    """
    objs: list[str] = []
    # Always place the portals (so the mode/size of the previous unit is not
    # inherited)
    objs.append(obj(MODE_PORTAL[mode], x - 4 * GRID, GROUND_TOP + 15.0))
    objs.append(obj(SIZE_MINI if mini else SIZE_NORM, x - 2 * GRID,
                    GROUND_TOP + 15.0))
    floor_top = GROUND_TOP + 120.0          # 210: well above the running line (head 120)
    apr = 4                                  # 4 cells of run-up floor (no ceiling)
    n = 4                                    # corridor length 4 cells = 120px
    for k in range(apr + n):
        cx = x + GRID / 2 + k * GRID
        objs.append(obj(BLOCK, cx, floor_top - GRID / 2))          # floor pillar
        if k >= apr:
            objs.append(obj(BLOCK, cx, floor_top + gap + GRID / 2))  # ceiling pillar
    u = {"x0": x, "x1": x + (apr + n) * GRID, "mode": mode, "mini": int(mini),
         "gap": gap, "floor_top": floor_top, "x_gapstart": x + apr * GRID,
         "inject_x": x + 1.5 * GRID,          # inject onto the run-up floor -> walks in
         "inject_y": floor_top + (9.0 if mini else 15.0)}
    return objs, x + (apr + n) * GRID, u


def build_crush() -> str:
    u"""Rig for measuring THE CONDITIONS FOR A CRUSH (squeeze) (2026-08-22).

    The model does not have lv22's x=20,130 dead end (a ~20px gap between floor
    1,179 and ceiling 1,200, an obj=NULL kill), so it currently needs a
    dedicated deadband. Turning it into a rule requires measuring the kill
    threshold of "gap vs height" and, when the player survives, the resolution
    (which way it is ejected). Every unit is independent (run with one injection
    per session).
    """
    UNITS.clear()
    PLAN.clear()
    # First sweep (gap 31..21, all survived) -> the death is not the outer box
    # but THE INNER BOX. In the second sweep, cube survives at 20.0 and dies at
    # 19.5, matching "dies on contact" with an inner-box half-height of 4.5.
    # This pass tightens the boundary and covers the other modes.
    parts: list[str] = []
    x = 600.0
    for g in [19.9, 19.75, 19.6, 19.5]:
        objs, x1, u = crush_unit(x, g, "cube", False)
        parts += objs
        UNITS.append(u)
        x = x1 + 10 * GRID
    for g in [20.5, 20.0, 19.75, 19.5]:
        objs, x1, u = crush_unit(x, g, "robot", False)
        parts += objs
        UNITS.append(u)
        x = x1 + 10 * GRID
    for g in [20.5, 20.0, 19.5, 19.0]:
        objs, x1, u = crush_unit(x, g, "ball", False)
        parts += objs
        UNITS.append(u)
        x = x1 + 10 * GRID
    # Mini cube: 12.5 died = the inner box is NOT scaled by 0.6.
    # If the inner-box half-height stays 4.5, death happens at gap <= 9+4.5 = 13.5
    for g in [14.5, 14.0, 13.6, 13.5]:
        objs, x1, u = crush_unit(x, g, "cube", True)
        parts += objs
        UNITS.append(u)
        x = x1 + 10 * GRID
    return header() + ";" + ";".join(parts) + ";"


def build_empty() -> str:
    """For triage: header only (no objects)."""
    return header() + ";"


def build_flat() -> str:
    """For triage: a flat floor and nothing else."""
    return header() + ";" + ";".join(floor_run(0, 6000, y=GRID / 2)) + ";"


def build_flat90() -> str:
    """For triage: align the floor's top surface with GD'S GROUND (y=90).

    empty (header only) ran to the end; flat (floor laid at y=15, i.e. 60px
    below the ground) died instantly. GD's ground is not y=0 but 90 (the player
    stands at y=105 = 90+15), so a block placed below the ground is buried, with
    the player inside it. Build calibration rigs ON TOP OF the ground.
    """
    return header() + ";" + ";".join(floor_run(0, 6000, y=75.0)) + ";"


def build_flatfar() -> str:
    """For triage: move the buried blocks away from the spawn (x>=300).

    Separates whether the instant death is "overlapping the spawn" or "the
    buried block itself".
    """
    return header() + ";" + ";".join(floor_run(300, 6000, y=GRID / 2)) + ";"


def build_sawcal() -> str:
    """Isolated circle hazards for outline sweeps (wave first; any mode later).

    Four stations 1000px apart, each one saw in open air at y=300, nothing else
    within 900px. Probes inject (x,y) around a station and bisect the kill
    boundary radially; the clean surroundings are the whole point -- lv20's own
    saws are all decorated with spikes/walls and every approach line is
    polluted (measured 2026-08-26: the polar sweep around uid1321 hit walls at
    R=30 in every direction).

      x=2000  id 1734  (lv20 big wheel, dumped radius 32)
      x=3000  id 1735  (lv20 small wheel, dumped radius 17.51)
      x=4000  id 1734 at scale 0.5 (does the kill radius scale?)
      x=5000  id 88    (classic saw family 88/89/98, the one the margins were
                        calibrated on)

    The rig starts as WAVE; the probe plan holds the input from t=0 so the
    natural path climbs away from the floor and the injection tick is free.
    Per-mode variants (sawcal_ship etc.) exist because the branch inside
    playerCircleCollision (0x211df0) and the player rect are per-mode; probes
    inject vy=0 so the injection tick is mode-agnostic.
    """
    return build_sawcal_mode("wave")


def build_sawcal_mode(mode: str, mini: bool = False) -> str:
    parts = [header(start_mode=mode, mini=mini)]
    parts += floor_run(0, 8000, y=GROUND_Y)
    # The no-input attempt slides along the flush floor ALIVE and completes the
    # level, and a completed session stops polling cmd.txt (the gd-probe trap).
    # A wall at x=230 kills every natural attempt; probe plans hold the input
    # from t=0 (climbing away over it is irrelevant -- they stop_at t=100,
    # x~130, before ever reaching it).
    for k in range(12):
        parts.append(obj(BLOCK, 230, 105 + GRID * k))
    parts.append(obj(1734, 2000, 300))
    parts.append(obj(1735, 3000, 300))
    parts.append(obj(1734, 4000, 300, scale=0.5))
    parts.append(obj(88, 5000, 300))
    # id 918 (type 47, radius 24): its kill circle is fetched via a VIRTUAL
    # "circle centre" (vtable +0x4a8 in playerCircleCollision), so it need not
    # sit on the anchor -- lv20's rot=-61 one kills 0.5..1.5px off the model's
    # prediction. One straight and one rotated to measure the offset.
    parts.append(obj(918, 6000, 300))
    parts.append(obj(918, 7000, 300, rot=-61))
    return ";".join(parts) + ";"


# ---- The same rig, with the saws GROUP-MOVED (2026-08-28) ------------------
# WHY. hazardHit (object.hpp) sends a moving circle down branch A (circle vs
# circle) and a static one down branch B (the player rect plus corner discs).
# The gate rests on ONE reading -- lv22 uid710, spider vs a group-moved saw --
# and lv21 contradicts it: 12 injection probes there (mini ship vs a rotated
# r=4 blade, full cube vs a moved r=24) ALL land on branch B, with A wrong in
# both directions. The disassembly says the branch is chosen by
# [[layer+0xdb0]+0x1cf] (0x211df0 @0x211e15), which is read off the LAYER and so
# cannot be a per-object property at all.
#
# Re-reading it in a real level failed: lv22's own circles sit in decorated
# columns and gd_death_context names four overlapping objects at once, exactly
# the pollution this rig family exists to avoid. So: the sawcal stations, with
# the saws group-moved, which reproduces lv21's condition in clean air and
# covers all 8 modes at once.
#
# THE LEVEL-STRING KEYS BELOW ARE NOT MEASURED. mklevel has never emitted a
# trigger, so 57 (group list), 51 (target group), 10 (duration), 28/29 (move
# x/y) and their units are taken from the community spec and MUST be closed by
# generating this rig, loading it, and reading the MOD's own trigger dump back
# (target / dur / ox / oy). The move offset's unit in particular is unverified,
# which is why the probe reads the saw's position out of grouptrace rather than
# predicting it.
TRIG_MOVE = 901
K_GROUPS, K_TARGET, K_DURATION, K_MOVE_X, K_MOVE_Y = 57, 51, 10, 28, 29
K_EASING, K_EASING_RATE = 30, 85
SAW_GROUP = 1

# One extra pair over the static rig: id 1582/1583 are lv21's r=4 blades, the
# objects the contradiction was measured on, and a small radius is the only way
# to separate the branches for the SPIDER (its rect half 13.5 exceeds r=4, so
# B's corner test cannot fire and B collapses to the bare rect -- a 4 px gap
# against A's 17.5).
SAWCAL_STATIONS = [(1734, 2000, 0.0), (1735, 3000, 0.0), (1734, 4000, 0.5),
                   (88, 5000, 0.0), (918, 6000, 0.0), (1582, 7000, 0.0),
                   (1583, 8000, 0.0)]


def build_sawcal_moved(mode: str, mini: bool = False) -> str:
    """sawcal with every saw carried by an autonomous Move trigger.

    The move is slow and vertical (the stations keep their x, so a dx=0 column
    -- where the two branches differ most -- is still reachable by injecting y
    alone), and long enough that the saws are STILL ANIMATING across the whole
    probe window rather than parked at the end of their travel.
    """
    parts = [header(start_mode=mode, mini=mini)]
    parts += floor_run(0, 9000, y=GROUND_Y)
    for k in range(12):
        parts.append(obj(BLOCK, 230, 105 + GRID * k))
    for oid, x, scale in SAWCAL_STATIONS:
        extra = {K_GROUPS: str(SAW_GROUP)}
        parts.append(obj(oid, x, 300, scale=scale, extra=extra))
    # x=45, NOT past the wall at 230. Probes reach a station by injecting x, and
    # the trigger has to have fired BEFORE that: the natural attempt dies on the
    # wall at x=211, so anything placed beyond it never fires at all (measured --
    # the first cut put this at 500 and the dump came back with the trigger read
    # correctly and the saws never moving). At 1x the player crosses x=45 around
    # t=35, well before any probe tick.
    #
    # 10 s of travel keeps the saws ANIMATING across every probe tick rather than
    # parked at the end. The displacement at any tick is small but not zero, so
    # the probe reads the saw's position out of grouptrace instead of predicting
    # it -- which also sidesteps the unverified unit of the move offset.
    parts.append(obj(TRIG_MOVE, 45, 300, extra={
        K_TARGET: str(SAW_GROUP),
        K_DURATION: "10",
        K_MOVE_X: "0",
        K_MOVE_Y: "120",
        K_EASING: "0",
        K_EASING_RATE: "2",
    }))
    return ";".join(parts) + ";"


# ---- A dual whose two halves are NOT in the same mode (2026-08-28) ---------
# WHY. The anchor seeds the second body's mode from the first
# (dp/src/dp/cli.hpp, `init.mode2 = init.mode`) because the mod's dump carried
# one mode column for the pair, and justifies it as "they differ for at most the
# tick between one clearing a mode portal's window and the other reaching it".
# With p2mode/p2vsize emitted that became measurable, and on lv16's solution the
# two never differ across 5,557 dual ticks -- so the assumption holds for the
# OFFICIAL corpus. It is not true in general: in custom levels, which this solver
# already clears cold, differing modes and sizes between the halves are ordinary.
#
# Rather than hunt for a custom level, build the case. A dual ship with no input
# separates on its own -- the first body falls to the floor, the second (flipped)
# climbs -- so a portal placed at FLOOR height is taken by one half and never
# reached by the other. Each station changes exactly one thing about the first
# body and leaves the second alone, which is what makes the dump columns readable
# as an answer.
#
# The third question the rig is for is whether the two can differ in X at all.
# The model gives the pair a single x (State::xAbs, which swapHalves does not
# swap), so that case would not be mis-modelled but unrepresentable. p2x is
# emitted alongside; this rig is where "always equal" gets its first evidence.
# ---- Force blocks (2026-08-30) ---------------------------------------------
# WHY. The push strength was a table keyed on m_uniqueID (level_loader.hpp), and
# a uid is level-specific by construction: a custom level's force block gets the
# default and is simply wrong. ForceBlockGameObject carries the strength as a
# member, and dividing the model's measured k by the m_force the game reports
# collapses the whole table:
#
#     uid     model k   m_force   k/m_force   mode (read off the g in each note)
#     11412    0.283      1.4      0.20214    robot   (-0.195)
#     14472    0.304      1.5      0.20267    robot   (-0.195)
#     17701    0.330      1.63     0.20245    robot   (-0.195)
#     14472    0.159      1.5      0.10600    ship    (-0.103)
#     17701    0.172      1.63     0.10552    ship    (-0.069)
#     carpet   0.108      1.2      0.09000    swing
#     17581    0.270      2 (max)  0.13500    ball
#
# Three independent objects agree to 0.26% for the robot and two to 0.5% for the
# ship, so dvy = m_force * C(mode) and the per-uid AND per-mode table was that
# one division written out longhand. C is known for four modes from lv22 alone.
# This rig is where the other four come from, and it also closes three things
# lv22 cannot answer with one instance each: whether C depends on scale (17701
# is 5.1x3.9 and agrees with the scale-1 boxes, but that is a single reading),
# whether m_force is linear (every value in lv22 is between 1.2 and 4.2), and
# the rot=180 mirror, which is an OPEN at both kFF2069 and kFF3645Max because
# lv22 has no force block that is rotated the other way.
#
# The keys come from ForceBlockGameObject::customObjectSetup (win 0x4c1d30),
# which indexes `values` (gd::string, 0x20 each) and `exists` (void*, 8 each) in
# parallel -- both divisions give the same six numbers, which is what makes them
# read rather than guessed.
FKEY_FORCE, FKEY_MIN, FKEY_MAX = 149, 526, 527
FKEY_RELATIVE, FKEY_RANGE, FKEY_ID = 528, 529, 530
FORCE_BOX, FORCE_FIELD = 2069, 3645

# (x, id, m_force, scale, rot, (min,max) when m_forceRange is set)
# Stations 1000px apart at y=300 with nothing within 900px: the sawcal geometry,
# for the same reason. The probes inject a state inside a box and read dvy, so a
# polluted approach line makes the reading meaningless. The default scale is 3
# (90x90) so an injected point sits well inside; the two scale stations are the
# ones that vary it.
FORCE_STATIONS = [
    (2000, FORCE_BOX,   1.0, 3.0,   0.0, None),          # baseline
    (3000, FORCE_BOX,   2.0, 3.0,   0.0, None),          # is m_force linear?
    (4000, FORCE_BOX,   1.0, 1.0,   0.0, None),          # does scale enter?
    (5000, FORCE_BOX,   1.0, 5.0,   0.0, None),          # ...at the other end
    (6000, FORCE_BOX,   1.0, 3.0, 180.0, None),          # the box mirror
    (7000, FORCE_FIELD, 1.0, 3.0, 180.0, (0.0, 2.0)),    # the lerp, as 17581
    (8000, FORCE_FIELD, 1.0, 3.0,   0.0, (0.0, 2.0)),    # the field mirror
]


def build_forcecal_mode(mode: str, mini: bool = False) -> str:
    """THIS RIG DOES NOT MEASURE THE FORCE. Kept for what it proves instead.

    Every station reads a push of EXACTLY ZERO under injected probes, in every
    mode. The field is not broken: the push is applied inside checkCollisions'
    object loop (0x215b4e), and an injection teleports the player 2,370 px in one
    tick, so the candidate list the loop walks was built somewhere else and does
    not contain the box. calib_forcedrop_* walks the player in instead and reads
    the field immediately.

    So this is the control that establishes "injection does not reach this
    mechanism" -- worth keeping, because the same reasoning applies to anything
    else resolved in that loop, and because a zero from an injected probe is
    otherwise indistinguishable from a zero force.
    """
    parts = [header(start_mode=mode, mini=mini)]
    parts += floor_run(0, 10000, y=GROUND_Y)
    # Same as sawcal: with no input the run slides along the flush floor ALIVE
    # and completes the level, and a completed session stops polling cmd.txt.
    for k in range(12):
        parts.append(obj(BLOCK, 230, 105 + GRID * k))
    for x, oid, force, scale, rot, rng in FORCE_STATIONS:
        extra = {FKEY_FORCE: f"{force:g}"}
        if rng is not None:
            extra[FKEY_RANGE] = "1"
            extra[FKEY_MIN] = f"{rng[0]:g}"
            extra[FKEY_MAX] = f"{rng[1]:g}"
        parts.append(obj(oid, x, 300, rot=rot, scale=scale, extra=extra))
    return ";".join(parts) + ";"


# The control for forcecal. The injected probes on that rig read a force of
# EXACTLY ZERO at every station, and an injection teleports the player 2,370 px
# in one tick -- the force is applied inside checkCollisions' object loop
# (0x215b4e), so a candidate list built for the old position would explain a
# zero without the field being wrong at all. Here the player reaches the box by
# walking: the floor stops at a ledge and the box straddles the fall, so nothing
# is injected and nothing is teleported. If dvy still does not move, the field
# is not firing for a reason that has nothing to do with the probes.
def build_forcedrop_mode(mode: str, mini: bool = False,
                         force: float = 1.0) -> str:
    """...and `force` is the whole point of the variants.

    dvy = m_force * |g| / D, and D could not be pinned from the m_force=1 rigs
    at all: the term is 0.09..0.225 there, GD's vy is on the 0.001 grid, so
    every reading carries 0.5-1% and the D backed out of the corpus scatters
    over 0.9556..0.9647 -- exactly that width. At m_force=10 the term is ~2.25
    and the same 0.001 is 0.04%, which separates those. The ladder of forces
    also settles whether m_force is linear at all, which no lv22 value shows
    (they run 1.2..4.2 and each is one point).
    """
    parts = [header(start_mode=mode, mini=mini)]
    # A gap from 300 to 700 with the floor resuming after it. The level still
    # has to be a level: the 439-char first cut (a ledge and nothing else) never
    # reached `serve: loaded` at all.
    parts += floor_run(0, 300, y=GROUND_Y)
    parts += floor_run(700, 4000, y=GROUND_Y)
    # 30 px grid, scale 10 -> 300x300 centred on the run height, so the fall off
    # the ledge at x=300 enters it at x=350 and leaves at x=650.
    parts.append(obj(FORCE_BOX, 500, GROUND_Y, scale=10.0,
                     extra={FKEY_FORCE: f"{force:g}"}))
    # AND THE WALL, past the box. The first cut left it out and the no-input
    # attempt COMPLETED the level (tick 3112, x=4390) -- which ends the session,
    # so every later run got "no serve: loaded" and the rig looked broken. It is
    # the same trap the sawcal note spells out; the wall has to be somewhere the
    # run reaches AFTER the thing being measured.
    # ...and it has to be TALL. At 12 blocks (435 px) the same trap came back for
    # m_force=10 only: f3 climbs into the wall, f10 sails over it and completes,
    # f30 leaves the world before it gets there -- so two rigs out of five failed
    # to load and the two looked like bad levels rather than one short wall.
    # 120 blocks clears any force this rig will be asked for.
    for k in range(120):
        parts.append(obj(BLOCK, 1000, 105 + GRID * k))
    return ";".join(parts) + ";"


# ---- Speed bands (2026-08-30) ----------------------------------------------
# cubePhysFor and ship_params both carry an UNMEASURED row for GD's 1.6 (the 4x
# portal), and the note at cubePhysFor says it "falls back" to the 0.7 row --
# which cannot be right, because the first branch there is `dxF > 1.78` and 4x is
# faster than 3x. One of the two is wrong and the level is the only place to ask.
#
# A plain floor and nothing else. The plan presses once, so one run gives the
# jump (the vy the press produces) and the gravity (the dvy of the arc after it)
# together, at whatever dx the game reports for the band.
# The one level the solver must REFUSE. kA22 is the platformer flag: identified
# by writing it into a working rig and asking the game (kA23 on the same rig
# leaves it an ordinary level, which is the control -- there the solver takes its
# normal path and reports that it cannot solve, rather than refusing).
#
# It is built from its own header rather than through a `platformer` argument to
# header(), deliberately. The note there says kA22-kA45 CHANGE THE PHYSICS, and a
# flag on the shared header is an invitation to set it on a physics rig and
# measure a different game.
def build_platformer() -> str:
    parts = [header(start_mode="cube") + ",kA22,1"]
    parts += floor_run(0, 4000, y=GROUND_Y)
    return ";".join(parts) + ";"


def build_speedcal(mode: str, speed: int, mini: bool = False) -> str:
    parts = [header(start_mode=mode, mini=mini, speed=speed)]
    parts += floor_run(0, 12000, y=GROUND_Y)
    # The wall is far out and tall: at 4x the run covers ground fast, and a
    # completed level ends the session (the trap the sawcal note spells out).
    for k in range(120):
        parts.append(obj(BLOCK, 6000, 105 + GRID * k))
    return ";".join(parts) + ";"


def build_dualmode() -> str:
    parts = [header(start_mode="ship", mini=False, dual=True)]
    parts += floor_run(0, 9000, y=GROUND_Y)
    # No wall here: the run has to REACH the stations, and the session ends by
    # completing the level rather than by dying on the way in.
    #
    # Every station sits at floor height. With no input the first body rests at
    # y=105 and the second climbs away, so the first takes them all and the
    # second keeps what it started with.
    parts.append(obj(P_CUBE, 1500, 105))      # p1 -> cube, p2 stays ship
    parts.append(obj(SIZE_MINI, 3000, 105))   # p1 -> mini, p2 stays normal
    parts.append(obj(P_BALL, 4500, 105))      # p1 -> ball
    parts.append(obj(P_SHIP, 6000, 105))      # ...and back, to see it swap twice
    return ";".join(parts) + ";"


def build_minhdr() -> str:
    """For triage: minimal header + floor. Which header key is causing trouble."""
    h = "kA2,0,kA3,0,kA4,0,kA6,1,kA7,1,kA8,0,kA10,0,kA13,0"
    return h + ";" + ";".join(floor_run(0, 6000, y=GRID / 2)) + ";"


# ---- Calibration rig for slope attachment conditions (2026-08-19) -------
PAD_YELLOW = 35

# Downhill floor slopes (probe measurement 2026-08-19: the solid is below the
# line in every case)
#   |m|=0.5: id1744 rot0 flipX  -> w60 h30, sy0=cy+15, sdir 2
#   |m|=1.0: id1743 rot0 flipX  -> w30 h30, sy0=cy+15, sdir 2
#   |m|=2.0: id1744 rot90 plain -> w30 h60, sy0=cy+30, sdir 4
DOWNRAMP = {0.5: (SLOPE60A, 0.0, True, False, 60.0, 30.0),
            1.0: (SLOPE30, 0.0, True, False, 30.0, 30.0),
            2.0: (SLOPE60A, 90.0, False, False, 30.0, 60.0)}

# The arc after a pad launch (fed in from a pass1 dump via --arc).
# (dt, dx, y, vy): tick relative to the launch tick / relative x / ABSOLUTE y / vy.
# The arc is identical for every unit (the same pad is entered in the same
# grounded state), so a single one suffices.
SL_ARC: list[tuple[int, float, float, float]] = []
SL_FIRE_DX = 0.0        # launch x - pad centre (measured in pass1)
SL_PITCH = 20 * GRID    # unit pitch. MUST NOT DEPEND ON ARC (identical in pass1/2)
SL_MODE = "cube"        # player mode for the slopeland family (ball for slopelandball)
SL4_SPEED = 0           # kA4 for slopeland4 (0=1x, 2=2x = the corpus' sp1.1)


def sl_pad_cx(k: int) -> float:
    return 90.0 + k * SL_PITCH + 6 * GRID


def load_arc(dump: Path) -> None:
    u"""Take the arc from a pass1 dump. The launch = the row where vy jumps by
    +8 or more in one tick. The rig takes zero input, so nothing but a pad can
    produce an upward jump beyond +8."""
    import csv as _csv
    global SL_FIRE_DX
    rows = []
    with dump.open(newline="", encoding="utf-8-sig", errors="replace") as f:
        for r in _csv.DictReader(f):
            try:
                rows.append((int(r["tick"]), float(r["x"]), float(r["y"]),
                             float(r["yvel"]), r.get("onGround", "0")))
            except (ValueError, KeyError):
                continue
    fire = None
    for i in range(1, len(rows)):
        if rows[i][3] - rows[i - 1][3] > 8.0 and rows[i][3] > 8.0:
            fire = i
            break
    if fire is None:
        raise SystemExit("--arc: no launch found (vy never jumps by +8)")
    t0, x0 = rows[fire][0], rows[fire][1]
    SL_FIRE_DX = x0 - sl_pad_cx(0)
    for i in range(fire, len(rows)):
        t, x, y, vy, og = rows[i]
        if i > fire and og == "1":
            break
        SL_ARC.append((t - t0, x - x0, y, vy))
    print(f"arc: {len(SL_ARC)} tick, fire_dx={SL_FIRE_DX:+.2f}, "
          f"apex={max(y for _, _, y, _ in SL_ARC):.1f}, "
          f"vy at the end {SL_ARC[-1][3]:.2f}")


def sl_place(m: float, vy_target: float, pH: float
             ) -> tuple[float, float, float, float, float] | None:
    u"""Place the slope at "the first descending point on the arc where vy drops
    to this value or below", so that its centre straddles the seat plane
    (line + pH*sqrt(1+m^2)). If the low end would reach the ground, walk back up
    the arc and re-place it at a shallower point. Returns (cx, cy, w, h, vyA)."""
    mm = abs(m)
    oid, rot, fx, fy, w, h = (DOWNRAMP if m < 0 else RAMP)[mm]
    idx = None
    for i, (dt, dx, y, vy) in enumerate(SL_ARC):
        if vy <= vy_target:
            idx = i
            break
    if idx is None:
        return None
    for i in range(idx, 0, -1):
        dt, dxA, yA, vyA = SL_ARC[i]
        if vyA >= 0:
            return None
        xa_rel = dxA                    # relative to the launch point
        xs_rel = xa_rel - 0.30 * w      # the straddle point is 30% from the left edge
        cx_rel = xs_rel + w / 2.0
        cy = (yA - pH * math.sqrt(1.0 + mm * mm)) - m * (xa_rel - cx_rel)
        if cy - mm * w / 2.0 >= GROUND_TOP + 3.0:
            return cx_rel, cy, w, h, vyA
    return None


def build_slopeland() -> str:
    u"""Calibration rig for SLOPE ATTACHMENT CONDITIONS. Zero input. cube only (v1).

    The foundation for lv16 t=5,923 (GD SNAPS A FALLING CUBE DOWNWARD onto a
    downhill slope) and for ceiling-ramp attachment (the most important census
    node). Exactly one quantity is measured: WHERE ON THE FALLING BOX GD SEATS
    THE PLAYER ON THE TICK THE LINE IS CROSSED (candidates: bottom-left corner /
    bottom of centre / bottom-right corner / centre against the seat plane). The
    spacing between candidates (2-23px) is wider than one tick of y (0.7-3.3px),
    so scattering the fractional part of the crossing with a phase delta lets a
    single run tell them apart.

    Unit = [yellow pad][arc][a downhill slope LEFT FLOATING]. No support is
    placed (the support's left face becomes a wall and kills). Once seated, the
    player slides off the low end back to the ground and walks to the next unit.

    Geometric constraint: unless the falling arc's gradient (vy*0.225/1.298
    px/px) is steeper than m, the player cannot land on a downhill slope (a
    shallow one diverges). So the vy target is per m: from -4 for |m|=0.5, from
    -7 for |m|=1, from -12.5 for |m|=2.
    A grazing intersection where the arc gradient ~ m has a relative approach of
    under 0.3px per tick = the sample that brackets the threshold most finely,
    so one is deliberately placed at the head of each m.

    BUILD IT IN TWO PASSES:
      pass1: no --arc = a pad-only rig. Run it and measure the arc
      pass2: rebuild with the slopes using --arc <the dump.csv from pass1>
    """
    objs: list[str] = []
    objs += floor_run(0, PAVE_X)
    if SL_MODE != "cube":
        objs.append(obj(MODE_PORTAL[SL_MODE], 45.0, GROUND_TOP + 15.0))
    combos: list[dict] = []
    targets = {0.5: (-4.0, -6.0, -9.0, -13.0),
               1.0: (-7.0, -9.0, -11.0, -13.0),
               2.0: (-12.5, -13.5, -14.5)}
    for mm, vys in targets.items():
        for vyt in vys:
            for d in (0.0, 0.4, 0.8):
                combos.append({"m": -mm, "vyt": vyt, "d": d})
    # The uphill control (the conditions for landing on an uphill slope from a fall)
    for mm in (0.5, 1.0):
        for d in (0.0, 0.4):
            combos.append({"m": mm, "vyt": -9.0, "d": d})
    pH = 15.0
    for k, c in enumerate(combos):
        x0 = 90.0 + k * SL_PITCH
        pad = sl_pad_cx(k)
        objs.append(obj(PAD_YELLOW, pad, GROUND_TOP + 5.0))
        u = {"x0": x0, "x1": x0 + SL_PITCH, "pad": pad, "mode": SL_MODE,
             "m": c["m"], "vyt": c["vyt"], "d": c["d"], "mini": 0}
        if SL_ARC:
            got = sl_place(c["m"], c["vyt"], pH)
            if got is None:
                print(f"  unit {k} (m={c['m']}, vy={c['vyt']}): "
                      f"no room on the arc -- slope skipped")
            else:
                cx_rel, cy, w, h, vyA = got
                cx = pad + SL_FIRE_DX + cx_rel
                cy += c["d"]
                oid, rot, fx, fy, _w, _h = (DOWNRAMP if c["m"] < 0
                                            else RAMP)[abs(c["m"])]
                objs.append(obj(oid, cx, cy, rot=rot, flip_x=fx, flip_y=fy))
                u.update({"cx": cx, "cy": cy, "w": w, "h": h, "vyA": vyA})
        UNITS.append(u)
    return header() + ";" + ";".join(objs) + ";"


def build_slopeland3() -> str:
    u"""Rig measuring THE RIDE-TIME DEPENDENCE OF THE RECTANGLE-TOP CLAMP.
    Zero input, cube, 1x.

    Measured on lv16: on the tick where the centre of a downhill ride crosses
    the top of the slope's rectangle (cy+hh), GD clamps to it for exactly 1
    tick -- but there are sites that do it (22 ticks into the ride,
    uid2292/2293/2865) and sites that do not (12 ticks in, uid2580). The
    geometry is a perfect clone (id/sdir/sup/rot/flip all identical), so the
    discriminator must be dynamic state = the ride-time hypothesis. The boundary
    is somewhere in (12, 22].

    Unit = [yellow pad][arc][2 chained downhill ramps (m=-0.5, w60)]. Placing
    the mid-air attachment height "0.649*K px above the top crossing" makes the
    ride at the crossing K ticks. The second of the pair is crossed too, so one
    unit yields two samples, K and K+46.
    Detection is y == cy+hh (GD emits a round value such as 270.000 for 1 tick).
    """
    objs: list[str] = []
    objs += floor_run(0, PAVE_X)
    combos = [(k, 0.0) for k in (4, 6, 8, 10, 12, 14, 16, 18, 20, 22, 24)]
    combos += [(12, 0.3), (16, 0.3), (20, 0.3)]
    pH = 15.0
    m = -0.5
    oid, rot, fx, fy, w, h = DOWNRAMP[0.5]
    for kk, c in enumerate(combos):
        K, d = c
        x0u = 90.0 + kk * SL_PITCH
        pad = sl_pad_cx(kk)
        objs.append(obj(PAD_YELLOW, pad, GROUND_TOP + 5.0))
        u = {"x0": x0u, "x1": x0u + SL_PITCH, "pad": pad, "K": K, "d": d,
             "mode": "cube", "m": m, "mini": 0}
        if SL_ARC:
            got = sl_place(m, -9.0, pH)
            if got is None:
                print(f"  unit {kk}: no room on the arc")
            else:
                # Taking sl_place's seat point (the x where the centre crosses
                # the seat plane) as the reference, re-derive the ramp's
                # vertical position so the ride up to the crossing is K ticks.
                # The line at the seat x: seat - pH*sqrt(1+m^2).
                # rTop = sy0 = seat y - 0.649K
                # So the seat x falls inside the span: x0 = xa - xoff - (33.54 - 1.298K)
                cx_rel, cy0, ww, hh, vyA = got
                dxA = None
                for dt, dx, y, vy in SL_ARC:
                    if vy <= -9.0:
                        dxA, yA = dx, y
                        break
                xa = pad + SL_FIRE_DX + dxA
                xoff = pH * math.tan(math.atan(0.5) / 2.0)
                xs1 = xa - xoff - (33.54 - 1.298 * K)
                sy0 = yA - 0.649 * K + d
                cy1 = sy0 - 15.0
                if cy1 - 15.0 - 30.0 - h / 2.0 < GROUND_TOP:
                    print(f"  unit {kk} (K={K}): too low, skipped")
                else:
                    objs.append(obj(oid, xs1 + w / 2.0, cy1, rot=rot,
                                    flip_x=fx, flip_y=fy))
                    objs.append(obj(oid, xs1 + w + w / 2.0, cy1 - 30.0,
                                    rot=rot, flip_x=fx, flip_y=fy))
                    u.update({"xs1": xs1, "cy1": cy1, "w": w,
                              "rtop1": cy1 + 15.0, "rtop2": cy1 - 15.0,
                              "yA": yA})
        UNITS.append(u)
    return header() + ";" + ";".join(objs) + ";"


def build_slopeland4() -> str:
    u"""RIG THAT SWEEPS THE RECTANGLE-TOP CLAMP GATE OVER |m| AND MODE
    (a generalisation of slopeland3).

    slopeland3 pinned m=-0.5 and cube and swept only K (the ride ticks up to the
    crossing), which produced the gate "descent >= pH*sqrt(1+m^2)". But THE
    CORPUS CONTRADICTS IT (findings 2026-08-19 night 3: the clamping cases
    8.07/14.53/28.55 and the pass-through cases 8.88/12.98/19.37/38.7 perfectly
    interleave by descent).
    This rig is 3 values of |m| x 6 values of K x 2 ramps, so |m| can be swept
    with the descent held fixed (the same descent occurs at a different K for
    each |m|).
    The ball version is `slopeland4ball`.

    Two passes: pass1 = no --arc (pad only) -> run it and capture the arc;
    pass2 = --arc <pass1 dump.csv>.
    """
    objs: list[str] = []
    objs += floor_run(0, PAVE_X)
    if SL_MODE != "cube":
        objs.append(obj(MODE_PORTAL[SL_MODE], 45.0, GROUND_TOP + 15.0))
    pH = 15.0
    # kA4: 0=1x (dx 1.298) / 2=2x (dx 1.6143). The corpus' sp1.1 is the latter.
    dx = 1.6143 if SL4_SPEED == 2 else 1.298
    vyt = {0.5: -4.0, 1.0: -7.0, 2.0: -12.5}
    combos: list[dict] = []
    for mm in (0.5, 1.0, 2.0):
        for K in (4, 8, 12, 16, 20, 24):
            combos.append({"mm": mm, "K": K})
    for kk, c in enumerate(combos):
        mm, K = c["mm"], c["K"]
        oid, rot, fx, fy, w, h = DOWNRAMP[mm]
        x0u = 90.0 + kk * SL_PITCH
        pad = sl_pad_cx(kk)
        objs.append(obj(PAD_YELLOW, pad, GROUND_TOP + 5.0))
        u = {"x0": x0u, "x1": x0u + SL_PITCH, "pad": pad, "K": K,
             "mode": SL_MODE, "m": -mm, "mini": 0,
             "descent": mm * dx * K}
        if SL_ARC:
            got = sl_place(-mm, vyt[mm], pH)
            if got is None:
                print(f"  unit {kk} (m=-{mm}, K={K}): no room on the arc")
            else:
                dxA = yA = None
                for dt, dxx, y, vy in SL_ARC:
                    if vy <= vyt[mm]:
                        dxA, yA = dxx, y
                        break
                xa = pad + SL_FIRE_DX + dxA
                xoff = pH * math.tan(math.atan(mm) / 2.0)
                # Re-place it at the vertical position that makes the ride up to
                # the crossing K ticks, so the seat x falls inside the span
                # (the generalisation of slopeland3)
                span = pH * math.sqrt(1.0 + mm * mm) / mm
                xs1 = xa - xoff - (span - dx * K)
                sy0 = yA - mm * dx * K
                cy1 = sy0 - h / 2.0
                if cy1 - h / 2.0 < GROUND_TOP + 3.0:
                    print(f"  unit {kk} (m=-{mm}, K={K}): too low, skipped")
                else:
                    objs.append(obj(oid, xs1 + w / 2.0, cy1, rot=rot,
                                    flip_x=fx, flip_y=fy))
                    u.update({"xs1": xs1, "cy1": cy1, "w": w, "h": h,
                              "rtop1": cy1 + h / 2.0, "yA": yA})
                    # A second ramp only if it fits (m=2 barely manages one)
                    if cy1 - h - h / 2.0 >= GROUND_TOP + 3.0:
                        objs.append(obj(oid, xs1 + w + w / 2.0, cy1 - h,
                                        rot=rot, flip_x=fx, flip_y=fy))
                        u["rtop2"] = cy1 - h / 2.0
        u["speed"] = SL4_SPEED
        UNITS.append(u)
    return header(SL_MODE, speed=SL4_SPEED) + ";" + ";".join(objs) + ";"


def build_slopeland4ball() -> str:
    u"""The ball version of slopeland4 (one ball portal at the head of the level)."""
    global SL_MODE
    SL_MODE = "ball"
    return build_slopeland4()


def build_slopeland4fast() -> str:
    u"""The 2x version of slopeland4 (dx 1.6143 = the corpus' sp1.1).

    On the rig (1x) the player does not fall at the |m|=0.5 seam, yet on the
    corpus' lv16@6,948 (m=-0.5, sp1.1) it does. This settles whether speed is
    the discriminator.
    """
    global SL4_SPEED
    SL4_SPEED = 2
    return build_slopeland4()


def build_slopelandball() -> str:
    u"""The ball version of slopeland (one ball portal at the head of the level).
    Measures whether a ball's attachment shares the cube's rules (seat plane
    +1.0 / entry side +0.5 / uphill asymmetry). The foundation for lv16 t=13,117
    (target 6) and lv22's ball corridor."""
    global SL_MODE
    SL_MODE = "ball"
    return build_slopeland()


def build_ceilpush() -> str:
    u"""Rig measuring HOW A CEILING RAMP PUSHES DOWN AND CARRIES AN UPRIGHT
    PLAYER. Zero input.

    The measurements at lv16 t=13,108-13,117 (ball) and lv18 t=3,007 (ship) both
    say "a descending ceiling ramp (sdir1) pushes on the head of an upright
    player and carries it at y = line - 12.728". 12.728 = 9*sqrt(2), which
    suggests the contact half-size is 9 (the player's box differs per purpose).
    Sweeping m separates 9*sqrt(1+m^2) from pH*sqrt(1+m^2) - constant. The
    release vy at the end of the carry (the ramp's low end) is measured at the
    same time (lv16 releases at -3.741).

    Unit = [yellow pad][arc][plateau (stacked blocks)][fall off the edge]
           [a descending ceiling ramp catches the head][release, down to ground].
    The plateau height is set by the apex of the pad's arc (a ball's arc is low,
    so at most 2 tiers).
    SL_MODE is switched to ball by ceilpushball.
    """
    objs: list[str] = []
    objs += floor_run(0, PAVE_X)
    if SL_MODE != "cube":
        objs.append(obj(MODE_PORTAL[SL_MODE], 45.0, GROUND_TOP + 15.0))
    blocks = 2 if SL_MODE == "ball" else 3
    ptop = GROUND_TOP + blocks * GRID          # top surface of the plateau
    mlist = (0.5, 1.0) if SL_MODE == "ball" else (0.5, 1.0, 2.0)
    combos = [{"m": mm, "d": d} for mm in mlist for d in (0.0, 0.4, 0.8)]
    for k, c in enumerate(combos):
        x0u = 90.0 + k * SL_PITCH
        pad = sl_pad_cx(k)
        objs.append(obj(PAD_YELLOW, pad, GROUND_TOP + 5.0))
        # The plateau: from a point clear of the arc's ascent, out to the edge
        px0, px1 = pad + 80.0, pad + 230.0
        xx = px0 + GRID / 2.0
        while xx < px1:
            for b in range(blocks):
                objs.append(obj(BLOCK, xx, GROUND_TOP + GRID / 2.0 + b * GRID))
            xx += GRID
        edge = px0 + GRID * ((px1 - px0) // GRID)   # the real edge (grid-rounded)
        mm = c["m"]
        oid, rot, fx, fy, w, h = CEIL_RAMP[mm]
        # Put the line's high end "just past the edge, 3px above the head"
        xs = edge + 6.0
        sy0 = ptop + 30.0 + 3.0 + c["d"]
        # CEIL_RAMP's line: sy0 (the high side, left) -> sy0 - m*w (right).
        # cy is the midpoint of that.
        cy = sy0 - mm * w / 2.0
        # Join A FLAT CEILING (underside = sy0) just before the ramp. Starting
        # with a bare ramp makes its left face a wall, killing on the tick the
        # head enters (measured on the first attempt). The lv16 site is also a
        # flat-ceiling-into-ramp join.
        xx = px1 - 2.5 * GRID
        while xx - GRID / 2.0 < xs:
            objs.append(obj(BLOCK, xx, sy0 + GRID / 2.0))
            xx += GRID
        objs.append(obj(oid, xs + w / 2.0, cy, rot=rot, flip_x=fx, flip_y=fy))
        UNITS.append({"x0": x0u, "x1": x0u + SL_PITCH, "pad": pad,
                      "mode": SL_MODE, "m": -mm, "d": c["d"], "mini": 0,
                      "edge": edge, "xs": xs, "w": w, "sy0": sy0,
                      "ptop": ptop})
    return header() + ";" + ";".join(objs) + ";"


def build_ceilpushball() -> str:
    global SL_MODE
    SL_MODE = "ball"
    return build_ceilpush()


MODE8 = ("cube", "ball", "robot", "spider", "ship", "ufo", "swing", "wave")


def ceilpush8_unit(x: float, mode: str, mm: float,
                   d: float = 0.0) -> tuple[list[str], float]:
    u"""One situation x one mode: "a descending ceiling ramp (sdir1) pushes down
    on the head of an upright player falling off an edge, carries it, and
    releases it at the low end". Zero input, non-lethal.

    [mode portal][run-up][3 uphill 0.5 ramps (+90px)][plateau (no gap = no
    launch)][edge][45px gap][descending ceiling ramp (sy0 = head height)]
    [release to the ground]

    Difference from ceilpush (the pad version): a pad's launch impulse differs
    per mode, so 8 modes cannot be put on the same plateau. Walking up ramps
    puts every mode at the same height (flyramps already proved zero-input rides
    for ship/UFO/swing; only wave may not follow the surface, so it is best
    effort).

    KEEP THE RAMP 45px FROM THE EDGE (measured in v1: placed at edge+6, the head
    entered the line at x=edge+13.8 while the feet were still on the plateau's
    edge, and the player was CRUSHED while grounded).
    45px = the box's half-width 15 (support ends) + 15 (until the box's right
    edge enters the ramp band) + margin. sy0 is at head height: on entry the
    player has already dropped a few px and does not touch, and the line
    descends (m*dx/tick) faster than the fall, so it catches up in mid-air.
    """
    objs: list[str] = []
    # Portal: partway along the run-up (well clear of the previous unit's
    # release, and after landing)
    objs.append(obj(MODE_PORTAL[mode], x + 4 * GRID, GROUND_TOP + 15.0))
    xr = x + 8 * GRID
    # 3 uphill 0.5 ramps (same placement as ramp_unit, +90px)
    oid, rot, fx, fy, w, h = RAMP[0.5]
    bottom = GROUND_TOP
    for i in range(3):
        cy = bottom + h / 2.0
        objs.append(obj(oid, xr + w / 2.0, cy, rot=rot,
                        flip_x=fx, flip_y=fy))
        yy = GROUND_TOP + GRID / 2
        while yy < cy - h / 2.0 + 1.0:
            for k in range(int(w / GRID)):
                objs.append(obj(BLOCK, xr + GRID / 2 + k * GRID, yy))
            yy += GRID
        bottom += h
        xr += w
    ptop = GROUND_TOP + 3 * h            # top surface of the plateau (180)
    # The plateau: joined to the top end WITH NO GAP = the player walks across
    # instead of launching (the converse of the note in ramp_unit: the plateau
    # version produced 0 launches)
    plen = 7 * GRID
    xx = xr + GRID / 2
    while xx < xr + plen:
        for b in range(3):
            objs.append(obj(BLOCK, xx, ptop - GRID / 2 - b * GRID))
        xx += GRID
    xe = xr + plen                        # the edge
    # Ceiling ramp (downhill, high side on the left). sy0 = ptop+30 = the head
    # height while on the plateau. The player drops a few px between losing
    # support at the edge and its box's right side reaching the ramp, so there
    # is no contact.
    coid, crot, cfx, cfy, cw, ch = CEIL_RAMP[mm]
    xs = xe + 45.0
    sy0 = ptop + 30.0 + d          # d: the offset that sweeps the release condition (ceilrel)
    objs.append(obj(coid, xs + cw / 2.0, sy0 - mm * cw / 2.0,
                    rot=crot, flip_x=cfx, flip_y=cfy))
    # After release the player falls to the ground (world 90). 10 cells to the
    # next unit
    return objs, xs + cw + 10 * GRID


def build_ceilpush8() -> str:
    u"""ONE SITUATION x ALL 8 MODES: the ceiling ramp's push-down (the
    upceil/ceilpush branch).

    A rig for validating the mode whitelists. The rule for this situation
    currently has three mode gates:
      - the upceil clamp value (r64: 0 for spider alone, -2.000 for ship/cube;
        ball/UFO/wave/robot are UNMEASURED)
      - the `c.mode != 7` on the upceil branch itself (excludes swing)
      - the {ship,swing,cube} whitelist on the adjacent-ceiling scan L8246 and
        the ridge scan L8930
    If GD behaves the same across all 8 modes, collapse them. If they split, the
    split becomes a measurement.
    """
    objs: list[str] = []
    x = 90.0
    # m=2 GOES LAST: measured in v2, when the line closes on a falling head at
    # 2.6px/tick GD KILLS instead of pushing down (the corpus' lv18 t=20,479 is
    # rising, closing at 0.69px/tick, and that one is pushed). A death ends the
    # rig there, so the 8 m=2 units are isolated at the back to protect the 16
    # units of {0.5,1} even if they die.
    # wave is excluded (measured in v3: wave dies in this rig's ceiling-ramp
    # band at x=7,583 and the rig ends there. The same "does not follow the
    # surface" that made flyramps drop wave)
    # m=2 is excluded too (measured in v3/v4: entering an m=2 downhill ceiling
    # from the side while falling makes GD KILL RATHER THAN PUSH DOWN -- cube
    # died at t=11,590 x=15,042 and everything after was lost. The corpus'
    # lv18 t=20,479 is the same m=-2 but rising, and there it is pushed.
    # The kill boundary cannot be measured on a rig (death = end of rig), so it
    # is done separately by worker injection)
    for mm in (0.5, 1.0):
        for mode in MODE8:
            if mode == "wave":
                continue
            x0 = x
            u, x = ceilpush8_unit(x, mode, mm)
            objs += u
            UNITS.append({"x0": x0, "x1": x, "mode": mode, "m": -mm,
                          "mini": 0})
    # [2026-08-21] KEEP THE LAST UNIT AWAY FROM THE END OF THE LEVEL. If it
    # reaches the end-of-level completion sequence, GD's dump freezes for ~170
    # ticks (the same x/y/rot row repeated; not a death). This turned the final
    # swing of ceilpush8 and ceilrel into a fake DIVERGE. A throwaway floor
    # block pushes levelMaxX 900px further out.
    objs.append(obj(BLOCK, x + 900.0, GROUND_Y))
    return header() + ";" + ";".join(objs) + ";"


def build_shortexit() -> str:
    u"""Rig measuring SHORT-STAY SLOPE LAUNCHES. Zero input, cube, 1x.

    The ramps rig uses 3-ramp chains, so it can only produce launches with a
    ride of 24+ ticks (= saturated). lv20 t=5,269 (target 2) lands on the top
    corner and is released at +2.762 after 1 tick -- the ramp factor's ramp-up
    (a stay of 1-24 ticks) had never been measured.

    Unit = [yellow pad][arc][a single floating uphill ramp]. The ramp is placed
    so the mid-air attachment x lands D px short of the top end. The player then
    leaves the top end after a ride of ~D/1.298 ticks, producing a launch vy.
    Sweeping D gives the factor(k) curve from a single run.
    Units with a small D land on the top corner itself depending on phase = they
    double as samples of target 2's "does not land until the penetration
    exceeds 1.000" side.
    """
    objs: list[str] = []
    objs += floor_run(0, PAVE_X)
    combos: list[dict] = []
    for mm in (0.5, 1.0):
        for D in (2.0, 4.0, 7.0, 10.0, 14.0, 19.0, 26.0, 34.0):
            for d in ((0.0, 0.4) if D <= 4.0 else (0.0,)):
                combos.append({"m": mm, "D": D, "d": d})
    pH = 15.0
    for k, c in enumerate(combos):
        x0u = 90.0 + k * SL_PITCH
        pad = sl_pad_cx(k)
        objs.append(obj(PAD_YELLOW, pad, GROUND_TOP + 5.0))
        u = {"x0": x0u, "x1": x0u + SL_PITCH, "pad": pad, "mode": "cube",
             "m": c["m"], "D": c["D"], "d": c["d"], "mini": 0}
        if SL_ARC:
            mm = c["m"]
            oid, rot, fx, fy, w, h = RAMP[mm]
            dxA = yA = None
            for dt, dx, y, vy in SL_ARC:
                if vy <= -9.0:
                    dxA, yA = dx, y
                    break
            xa = pad + SL_FIRE_DX + dxA          # the centre x we want to seat at
            xs = xa - (w - c["D"])               # top end x1 = xa + D
            cx = xs + w / 2.0
            cy = (yA - pH * math.sqrt(1.0 + mm * mm)) \
                - mm * (xa - cx) + c["d"]
            if cy - mm * w / 2.0 < GROUND_TOP + 3.0:
                print(f"  unit {k}: too low, skipped")
            else:
                objs.append(obj(oid, cx, cy, rot=rot, flip_x=fx, flip_y=fy))
                u.update({"cx": cx, "cy": cy, "w": w,
                          "xtop": xs + w, "ytop": cy + mm * w / 2.0})
        UNITS.append(u)
    return header() + ";" + ";".join(objs) + ";"


def build_edgeland() -> str:
    u"""Rig measuring HOW FAR PAST A PLATFORM'S EDGE THE PLAYER CAN STILL LAND
    (pInner). Zero input.

    lv19 t=15,514: with the falling cube's centre 7.33px past the platform's
    right edge, GD does not catch the platform. lv20 t=5,269: ~12.7px past the
    edge of a width-1.5 bar is not caught either. The boundary of how far it
    will still catch is unmeasured (estimated 5-6).

    Unit = [yellow pad][arc][a single floating block]. The block is placed so
    that the centre x on the tick where the falling bottom crosses the top
    surface is "the right edge + O". Sweeping O brackets the landing /
    pass-through boundary. On a pass-through the player just falls to the ground
    and walks on.
    """
    objs: list[str] = []
    objs += floor_run(0, PAVE_X)
    combos: list[dict] = []
    for O in (2.0, 4.0, 5.0, 6.0, 7.0, 8.0, 9.0, 10.0, 12.0):
        for d in ((0.0, 0.4) if 5.0 <= O <= 9.0 else (0.0,)):
            combos.append({"O": O, "d": d})
    pH = 15.0
    for k, c in enumerate(combos):
        x0u = 90.0 + k * SL_PITCH
        pad = sl_pad_cx(k)
        objs.append(obj(PAD_YELLOW, pad, GROUND_TOP + 5.0))
        u = {"x0": x0u, "x1": x0u + SL_PITCH, "pad": pad, "mode": "cube",
             "O": c["O"], "d": c["d"], "mini": 0}
        if SL_ARC:
            dxA = yA = None
            for dt, dx, y, vy in SL_ARC:
                if vy <= -9.0:
                    dxA, yA = dx, y
                    break
            xa = pad + SL_FIRE_DX + dxA        # centre x around where the bottom crosses
            face = yA - pH + c["d"]            # top surface = the bottom at the crossing
            cx = xa - c["O"] - GRID / 2.0      # right edge = xa - O
            cy = face - GRID / 2.0
            objs.append(obj(BLOCK, cx, cy))
            u.update({"cx": cx, "cy": cy, "face": face,
                      "edge": cx + GRID / 2.0})
        UNITS.append(u)
    return header() + ";" + ";".join(objs) + ";"


def recttop_unit(x: float, mode: str, m: float, mini: bool, n_down: int
                 ) -> tuple[list[str], float, dict]:
    u"""One unit of [portal][climb an uphill ramp][plateau][n downhill ramps]
    [run through]. Zero input.

    Measures ON WHICH CROSSING THE RECTANGLE-TOP CLAMP (sp->cy+sp->hh) HAPPENS.
    The player walks from the plateau onto the high end of the first downhill
    ramp and seats there (descent 0). From then on it crosses rTop exactly once
    per ramp, so one unit yields n_down CROSSING SAMPLES. The point is that the
    ramp index (its own ramp, or one past a seam) is what varies -- since the
    descent has been ruled out as the discriminator (findings 2026-08-19
    night 3), this is the next candidate.
    """
    objs: list[str] = []
    oidU, rotU, fxU, fyU, wU, hU = RAMP[1.0]      # the climb is always |m|=1 (30x30)
    oidD, rotD, fxD, fyD, wD, hD = DOWNRAMP[m]
    top = GROUND_TOP
    objs.append(obj(MODE_PORTAL[mode], x, top + 15.0))
    objs.append(obj(SIZE_MINI if mini else SIZE_NORM, x + 2 * GRID,
                    top + 15.0))
    xr = x + 8 * GRID
    # The climb: gain the height the descent will use (hD*n_down, in steps of hU=30)
    climb = int(math.ceil(n_down * hD / hU))
    bottom = top
    for _ in range(climb):
        cy = bottom + hU / 2.0
        objs.append(obj(oidU, xr + wU / 2.0, cy, rot=rotU,
                        flip_x=fxU, flip_y=fyU))
        yy = top + GRID / 2
        while yy < cy - hU / 2.0 + 1.0:
            for k in range(int(wU / GRID)):
                objs.append(obj(BLOCK, xr + GRID / 2 + k * GRID, yy))
            yy += GRID
        bottom += hU
        xr += wU
    # Plateau (top surface = bottom). Just 4 cells of walking to settle the
    # speed and the grounded state
    for k in range(4):
        yy = bottom - GRID / 2
        while yy > top - 1.0:
            objs.append(obj(BLOCK, xr + GRID / 2 + k * GRID, yy))
            yy -= GRID
    xr += 4 * GRID
    x_seat = xr
    # n downhill ramps. Placed so the first one's high end (the line at x0) is
    # at the same height as the plateau's top surface (for DOWNRAMP,
    # sy0 = cy + h/2).
    crossings = []
    hi = bottom
    for i in range(n_down):
        cy = hi - hD / 2.0
        objs.append(obj(oidD, xr + wD / 2.0, cy, rot=rotD,
                        flip_x=fxD, flip_y=fyD))
        # Fill in under the ramp (to stop the player falling through)
        yy = cy - hD / 2.0 - GRID / 2
        while yy > top - 1.0:
            for k in range(int(wD / GRID)):
                objs.append(obj(BLOCK, xr + GRID / 2 + k * GRID, yy))
            yy -= GRID
        crossings.append({"idx": i + 1, "rtop": cy + hD / 2.0,
                          "x0": xr, "x1": xr + wD})
        hi -= hD
        xr += wD
    # Run through: lay a floor at the height the descent ended on, out to the
    # next unit
    for k in range(6):
        yy = hi - GRID / 2
        while yy > top - 1.0:
            objs.append(obj(BLOCK, xr + GRID / 2 + k * GRID, yy))
            yy -= GRID
    xr += 6 * GRID
    u = {"x0": x, "mode": mode, "m": m, "mini": int(mini),
         "n_down": n_down, "x_seat": x_seat, "plateau_top": bottom,
         "crossings": crossings}
    return objs, xr + 4 * GRID, u


def build_recttop() -> str:
    u"""Rig measuring THE GATE ON THE RECTANGLE-TOP CLAMP (sp->cy+sp->hh).
    Zero input.

    The existing gate is "descent since attaching >= pH*sqrt(1+m^2)", but that
    was settled on 2026-08-19 as NOT BEING THE DISCRIMINATOR (the clamping cases
    8.07 / 14.53 / 28.55 and the pass-through cases 8.88 / 12.98 / 19.37 / 38.7
    interleave perfectly).
    Here THE RAMP INDEX is swept (the ramp it seated on, versus one past a
    seam). One unit yields n_down crossings, so cube/ball x 3 values of |m| x
    normal/mini x n_down=4 gives 96 samples per run.

    Detection: pick the ticks where y is exactly rtop (a round value) out of the
    dump.
    """
    objs: list[str] = []
    objs += floor_run(0, PAVE_X)
    x = 90.0
    for mode in ("cube", "ball"):
        for m in (1.0, 0.5, 2.0):
            for mini in (False, True):
                u_objs, x, u = recttop_unit(x, mode, m, mini, 4)
                objs += u_objs
                UNITS.append(u)
    return header() + ";" + ";".join(objs) + ";"


def build_portwavebig() -> str:
    u"""The NORMAL-SIZE WAVE version of portwave. Zero input.

    At lv19 t=19,491 (a normal wave passing through the rot=-41 speed portal
    id200) the model fires 1 tick early. The mini wave's contact half-size
    matches 4/5 on portwave, so this measures THE NORMAL SIZE'S CONTACT
    HALF-SIZE in isolation.
    The angle is swept finely (the more the portal is rotated, the more the x
    half and the y half mix).
    """
    objs: list[str] = []
    x = 90.0
    for rot in (0, 10, 20, 30, 41, 43, 50, 60, 70, 90):
        x0 = x
        objs.append(obj(P_CUBE, x, GROUND_TOP + 15.0))
        objs.append(obj(SIZE_NORM, x + 2 * GRID, GROUND_TOP + 15.0))
        objs.append(obj(P_WAVE, x + 4 * GRID, GROUND_TOP + 15.0))
        x_port = x + 14 * GRID
        objs.append(obj(P_UFO, x_port, GROUND_TOP + 15.0, rot=float(rot)))
        x = x_port + 24 * GRID
        UNITS.append({"x0": x0, "x1": x, "rot": rot, "x_port": x_port,
                      "mode": "ufo", "m": 0, "mini": 0, "player": "wave"})
    objs += floor_run(0, PAVE_X)
    return header() + ";" + ";".join(objs) + ";"


def build_flyramps() -> str:
    """Calibration rig for RAMP RIDES/LAUNCHES IN THE FLIGHT MODES. Clears every
    unit with zero input.

    The flight version of `ramps` (the 4 ground modes). With no input, ship /
    UFO / swing simply sink under gravity, so the same `ramp_unit` as the ground
    modes works unchanged (they slide along the floor into the ramp, get pushed
    up by the surface, climb, and launch at the top end).
    wave was dropped because it bounces instead of following the surface.

    THE AIM: the flight-mode ramp families still in census
      lv16 `m1/mini1/g1/gdg1/sp0.7/slope+0.50/ride0`  (ship mini)
      lv18 `m1/mini1/g1/gdg1/sp1.1/slope-1.00/ride2`  (ship mini)
      lv21 `m3/mini1/g1/gdg1/sp1.1/slope-2.00/ride9`  (UFO mini)
      lv22 `m7/mini0/g1/gdg0/sp1.1/slope+1.00/ride3`  (swing)
    Samples that occur only once each across the 22 official levels are taken as
    3 modes x 3 values of |m| x normal/mini x 2 ride lengths = 36 units in a
    single run.
    """
    objs: list[str] = []
    x = 90.0
    for mode in ("ship", "ufo", "swing"):
        for m in (1.0, 0.5, 2.0):
            for mini in (False, True):
                for n, bury in ((3, 0.0), (1, 0.0)):
                    x0 = x
                    u, x = ramp_unit(x, mode, m, mini, n, bury)
                    objs += u
                    UNITS.append({"x0": x0, "x1": x, "mode": mode, "m": m,
                                  "mini": int(mini), "ramps": n,
                                  "bury": bury})
    objs += floor_run(0, PAVE_X)
    return header() + ";" + ";".join(objs) + ";"


CR_SPEED = 0   # kA4 for ceilrel (0=1x=sp0.9, 1=0.5x=sp0.7, 2=2x=sp1.1)


def build_ceilrel() -> str:
    u"""Rig that cracks the release-vy ASSIGNMENT on an m=1 downhill ceiling.

    First measurement from ceilpush8: on the tick after release at the low end
    of an m=1 ramp, GD assigns vy (ship -4.628 / swing -4.628 / UFO -4.397; the
    preceding vy differs yet ship=swing, so it is an assignment, not a delta).
    d (a height offset applied to the whole ramp) changes the y, the falling
    speed and the ride length at release, so one run decides whether the
    assigned value
      - is invariant in d -> a per-mode constant (a function of m and dx)
      - moves with d      -> kinematic (a function of the state at release)
    The speed is CR_SPEED (per rig, kA4).
    """
    objs: list[str] = []
    # [2026-08-21] Change the speed WITH A REAL SPEED PORTAL, NOT VIA THE HEADER
    # (kA4). The header does not appear in objrects, so the model ran at 1x and
    # the whole rig came out as a fake DIVERGE (the first run of ceilrel11/07).
    # id 200=0.7 / 202=1.1 (type 20) are measured from the corpus (lv19 uid13186
    # and others).
    if CR_SPEED == 1:
        objs.append(obj(200, 45.0, GROUND_TOP + 15.0))
    elif CR_SPEED == 2:
        objs.append(obj(202, 45.0, GROUND_TOP + 15.0))
    x = 90.0
    for mode in ("ship", "ufo", "swing"):
        for d in (0.0, 4.0, 8.0, 12.0):
            x0 = x
            u, x = ceilpush8_unit(x, mode, 1.0, d)
            objs += u
            UNITS.append({"x0": x0, "x1": x, "mode": mode, "m": -1.0,
                          "mini": 0, "d": d, "speed": CR_SPEED})
    objs.append(obj(BLOCK, x + 900.0, GROUND_Y))   # end-of-level freeze guard (as above)
    return header() + ";" + ";".join(objs) + ";"


def build_ceilrel11() -> str:
    global CR_SPEED
    CR_SPEED = 2
    return build_ceilrel()


def build_ceilrel07() -> str:
    global CR_SPEED
    CR_SPEED = 1
    return build_ceilrel()


SEAM_SPEED = 0   # 0=1x / 1=0.7x / 2=1.1x (set by seam07 / seam11)


def seam_unit_v3(x: float, mode: str, m: float, delta: float, n_dn: int
                 ) -> tuple[list[str], float, dict]:
    u"""One seam unit that RIDES A DOWNHILL CHAIN INTO A FLUSH SLAB (v3).
    Zero input.

    Two facts learned in v1/v2:
      1. Joining a downhill ramp directly to the top of an uphill one makes GD
         LAUNCH AT THE APEX (cube +7.405 / ball +5.554 = the slopeExitVy of that
         gradient). Interposing a plateau prevents the launch
      2. YOU CANNOT WALK FROM A PLATEAU ONTO A DOWNHILL RAMP -- by the time the
         contact point is inside the span, the seat plane is running away at
         1.3|m| px/tick. The player free-falls off the plateau's edge and
         ATTACHES WHERE THE FALL CATCHES UP WITH THE SEAT PLANE. Catching up
         takes n = |m|*dx/(0.225*g) ticks (35px for cube m=0.5, 69px for m=1;
         about twice that for ball), so the key is to MAKE THE DOWNHILL CHAIN
         LONGER THAN THAT. v2 used a single downhill ramp (30px), so at m=1/2 it
         never caught up and passed straight through
      3. The idea of using the apex launch to drop the arc onto the downhill
         (v3) was rejected: the landing is 162px away for cube m=1 and 679px for
         |m|=2, and the landing itself splits at the 9px level via a different
         mechanism (the slopeland family), fouling the seam measurement

    v4 = [n_dn+1 uphill ramps][plateau (suppresses the launch)][n_dn downhill
    ramps][flush slab].
    """
    oid_u, rot_u, fxu, fyu, w, h = RAMP[m]
    oid_d, rot_d, fxd, fyd, _w, _h = DOWNRAMP[m]
    objs: list[str] = []
    objs.append(obj(MODE_PORTAL[mode], x, GROUND_TOP + 15.0))
    objs.append(obj(SIZE_NORM, x + 2 * GRID, GROUND_TOP + 15.0))
    n_up = n_dn + 1                      # one extra so the slab stays above the ground
    xr = x + 8 * GRID + delta
    bottom = GROUND_TOP
    for i in range(n_up):
        cy = bottom + h / 2.0
        objs.append(obj(oid_u, xr + w / 2.0, cy, rot=rot_u,
                        flip_x=fxu, flip_y=fyu))
        yy = GROUND_TOP + GRID / 2
        while yy < cy - h / 2.0 + 1.0:
            for k in range(int(w / GRID)):
                objs.append(obj(BLOCK, xr + GRID / 2 + k * GRID, yy))
            yy += GRID
        bottom += h
        xr += w
    # The plateau at the apex: without it GD launches at the apex (measured in v1)
    plat_len = 6 * GRID
    xx = xr + GRID / 2
    while xx < xr + plat_len:
        yy = GROUND_TOP + GRID / 2
        while yy <= bottom - GRID / 2 + 0.01:
            objs.append(obj(BLOCK, xx, yy))
            yy += GRID
        xx += GRID
    xr += plat_len
    for i in range(n_dn):
        cy = bottom - h / 2.0
        objs.append(obj(oid_d, xr + w / 2.0, cy, rot=rot_d,
                        flip_x=fxd, flip_y=fyd))
        # [v5] DO NOT FILL UNDER A DOWNHILL RAMP. The bottom edge of a downhill
        # ramp's box is "the height of the low end", so filling it the same way
        # as an uphill ramp puts the fill's top surface ABOVE THE LINE over most
        # of the span. GD ignores that and rides the line, while the model
        # landed on the fill's top surface and dropped the ride (this made
        # seam11's fast cube column and every ball column a fake DIVERGE). The
        # ramp itself provides the solid side, so nothing falls through without
        # the fill.
        bottom -= h
        xr += w
    x1 = xr                              # low end of the downhill chain = the seam
    seam_y = bottom                      # the line height there = the slab's top surface
    slab_len = 12 * GRID
    xx = x1 + GRID / 2
    while xx < x1 + slab_len:
        yy = GROUND_TOP + GRID / 2
        while yy <= seam_y - GRID / 2 + 0.01:
            objs.append(obj(BLOCK, xx, yy))
            yy += GRID
        xx += GRID
    u = {"x0": x, "mode": mode, "m": -m, "mini": 0, "delta": delta,
         "x1": x1, "seam_y": seam_y, "w": w, "h": h, "n_dn": n_dn}
    return objs, x1 + slab_len + 14 * GRID, u


def seam_unit(x: float, mode: str, m: float, delta: float
              ) -> tuple[list[str], float, dict]:
    u"""One seam unit of DOWNHILL RAMP -> FLUSH SLAB. Zero input.

    r79 established that "if what follows the end is a flush slab, the ride
    survives until the tick where *the trailing corner (centre - (pH - xoff)) +
    c* leaves the end", but the corpus' 3 samples only bracketed
    c in (1.016, 1.23] (the implementation provisionally uses 1.1). Here the
    same seam is laid out with THE PHASE delta STEPPED, so each unit yields one
    interval for c from the tick of the release pulse (vy := m*dx/0.25 for 1
    tick).

    [mode][size][2 uphill][A FLUSH PLATEAU][1 downhill][flush slab][gap]

    delta shifts the whole ramp assembly (uphill, plateau, downhill, slab, fill)
    in x = it shifts the phase at which the player arrives. THE SLAB IS JOINED
    AT THE SAME HEIGHT AS THE DOWNHILL RAMP'S LOW END (that is the definition of
    "flush": the implementation tests "is there a solid at end +1px whose top
    surface is within +/-0.5 of the line at the end").

    A PLATEAU IS REQUIRED PAST THE UPHILL APEX (the v1 failure): joining a
    downhill ramp directly to the top of an uphill one makes GD launch at the
    apex (cube +7.405 / ball +5.554 = the slopeExitVy of that gradient) and the
    player flies over the seam and lands beyond it -- all 48 units were
    measuring "the apex launch". As the note on the ramps rig says, JOINING A
    PLATEAU AT THE SAME HEIGHT AS THE TOP END WITH NO GAP PREVENTS THE LAUNCH,
    so let the player walk over that before entering the downhill.
    """
    oid_u, rot_u, fxu, fyu, w, h = RAMP[m]
    oid_d, rot_d, fxd, fyd, _w, _h = DOWNRAMP[m]
    objs: list[str] = []
    objs.append(obj(MODE_PORTAL[mode], x, GROUND_TOP + 15.0))
    objs.append(obj(SIZE_NORM, x + 2 * GRID, GROUND_TOP + 15.0))
    n_up = 2
    xr = x + 8 * GRID + delta
    bottom = GROUND_TOP
    for i in range(n_up):
        cy = bottom + h / 2.0
        objs.append(obj(oid_u, xr + w / 2.0, cy, rot=rot_u,
                        flip_x=fxu, flip_y=fyu))
        yy = GROUND_TOP + GRID / 2
        while yy < cy - h / 2.0 + 1.0:
            for k in range(int(w / GRID)):
                objs.append(obj(BLOCK, xr + GRID / 2 + k * GRID, yy))
            yy += GRID
        bottom += h
        xr += w
    # The plateau at the apex (top surface bottom). It suppresses the launch and
    # lets the player "walk" into the downhill
    plat_len = 6 * GRID
    xx = xr + GRID / 2
    while xx < xr + plat_len:
        yy = GROUND_TOP + GRID / 2
        while yy <= bottom - GRID / 2 + 0.01:
            objs.append(obj(BLOCK, xx, yy))
            yy += GRID
        xx += GRID
    xr += plat_len
    # One downhill ramp: the line runs bottom -> bottom - h
    cy_d = bottom - h / 2.0
    objs.append(obj(oid_d, xr + w / 2.0, cy_d, rot=rot_d,
                    flip_x=fxd, flip_y=fyd))
    yy = GROUND_TOP + GRID / 2
    while yy < cy_d - h / 2.0 + 1.0:
        for k in range(int(w / GRID)):
            objs.append(obj(BLOCK, xr + GRID / 2 + k * GRID, yy))
        yy += GRID
    x1 = xr + w                      # right edge of the downhill ramp = the seam
    seam_y = bottom - h              # the line height at the low end = the slab's top
    # The flush slab (top surface seam_y). ALIGN ITS LEFT EDGE WITH x1
    # (aligning centres sticks out 15px too early and makes the seam a lie --
    # the same trap as ceil_unit L465)
    slab_len = 12 * GRID
    xx = x1 + GRID / 2
    while xx < x1 + slab_len:
        yy = GROUND_TOP + GRID / 2
        while yy <= seam_y - GRID / 2 + 0.01:
            objs.append(obj(BLOCK, xx, yy))
            yy += GRID
        xx += GRID
    u = {"x0": x, "mode": mode, "m": -m, "mini": 0, "delta": delta,
         "x1": x1, "seam_y": seam_y, "w": w, "h": h}
    return objs, x1 + slab_len + 14 * GRID, u


def build_seam() -> str:
    u"""Seam rig: bracket the release tick of downhill -> flush slab by phase
    (pins down the c of r79).

    2 modes (cube/ball -- the same cast as the corpus' 3 samples) x 2 values of
    |m| x 12 phase points = 48 units. delta runs 0..1.21 in steps of 0.11
    (covering one tick of movement, 1.298).
    Each unit yields one interval for c from the release tick, so the phases
    intersect to bracket it.
    |m|=2 is left out because the apex launch's arc does not land until 679px
    later (see the v3 note).
    """
    objs: list[str] = []
    # Set the speed with a real portal (the header's kA4 does not appear in
    # objrects -- the lesson from ceilrel)
    if SEAM_SPEED == 1:
        objs.append(obj(200, 45.0, GROUND_TOP + 15.0))
    elif SEAM_SPEED == 2:
        objs.append(obj(202, 45.0, GROUND_TOP + 15.0))
    x = 90.0
    # The downhill chain's length is set by "the distance in which a player
    # falling off the plateau catches up with the seat plane"
    # (35px for cube m=0.5 / 69px for m=1; about twice that for ball; scaling
    # with dx^2 in the speed) + margin
    n_down = {1.0: 6, 0.5: 4}
    for mode in ("cube", "ball"):
        for m in (1.0, 0.5):
            for k in range(12):
                delta = round(k * 0.11, 2)
                u_objs, x_next, u = seam_unit_v3(x, mode, m, delta, n_down[m])
                objs += u_objs
                u["x1_unit"] = x_next
                u["speed"] = SEAM_SPEED
                UNITS.append(u)
                x = x_next
    # In the unit table, x1 means the unit boundary (calib_units), so move the
    # seam's x1 into seam_x1 before overwriting it
    for u in UNITS:
        u["seam_x1"] = u["x1"]
        u["x1"] = u.pop("x1_unit")
    objs.append(obj(BLOCK, x + 900.0, GROUND_Y))   # end-of-level freeze guard
    return header() + ";" + ";".join(objs) + ";"


def ridestep_unit(x: float, mode: str, m: float, dz: float
                  ) -> tuple[list[str], float, dict]:
    u"""A STEP DURING A RIDE: while riding an uphill ramp, run into a block
    whose top surface is dz px above the line. Measures whether GD transfers
    onto the step or passes through on the line.

    The corpus' 2 samples only bracket the boundary; everything between is
    unmeasured:
      lv16 t=5,378  feet 0.219px below the surface -> GD DOES mount (y=341.000)
      lv19 t=245    feet 5.5px below the surface   -> GD DOES NOT (stays on the line)
    The model uses landTol=0.001 during a ride (a guess), so it drops the former.
    """
    oid_u, rot_u, fxu, fyu, w, h = RAMP[m]
    objs: list[str] = []
    objs.append(obj(MODE_PORTAL[mode], x, GROUND_TOP + 15.0))
    objs.append(obj(SIZE_NORM, x + 2 * GRID, GROUND_TOP + 15.0))
    n_up = 3
    xr = x + 8 * GRID
    bottom = GROUND_TOP
    for i in range(n_up):
        cy = bottom + h / 2.0
        objs.append(obj(oid_u, xr + w / 2.0, cy, rot=rot_u,
                        flip_x=fxu, flip_y=fyu))
        yy = GROUND_TOP + GRID / 2
        while yy < cy - h / 2.0 + 1.0:
            for k in range(int(w / GRID)):
                objs.append(obj(BLOCK, xr + GRID / 2 + k * GRID, yy))
            yy += GRID
        bottom += h
        xr += w
    # PUT THE STEP OUTSIDE THE RAMP'S BOX. The v2 failure: a step buried inside
    # the box was completely ignored by GD (28/28 passed through) -- lv16's step
    # uid1935 sits BELOW the next ramp's box, and only there does it act as a
    # "step".
    # Its top surface goes just below the second box's bottom edge (-0.1), and
    # SWEEPING x sets the reach at the contact tick (step top - feet) to dz.
    #   reach = T - line1(X - 15 + xoff), where T = the box's bottom edge - 0.1
    # [v4] What is swept is depth = line(the step's x) - the step's top surface.
    # In the v3 measurement, that is what decides whether GD mounts the step
    # (2.829 passes through / 3.229 mounts). It is a pure geometric quantity,
    # unpolluted by phase (the fractional part of the contact tick).
    # depth = m*(15-xoff) - dz.
    x_r1 = x + 8 * GRID + w                      # left edge of the 2nd ramp (the ridden face)
    top = GROUND_TOP + 2 * h - 0.1               # just below the 3rd ramp box's bottom edge
    xoff = 15.0 * math.tan(math.atan(abs(m)) / 2.0)
    x_step = x_r1 + 15.0 - xoff + (h - 0.1 - dz) / abs(m)
    objs.append(obj(BLOCK, x_step + GRID / 2, top - GRID / 2))
    u = {"x0": x, "mode": mode, "m": m, "mini": 0, "dz": dz,
         "x_step": x_step, "step_top": top, "ramp_x0": x + 8 * GRID,
         "x_r1": x_r1}
    return objs, xr + 16 * GRID, u


def build_ridestep() -> str:
    u"""Rig bracketing the tolerance for landing on a step during a ride.
    cube x |m|{1,0.5} x 14 values of dz = 28.

    PUT THE LARGER dz LATER (a deep step risks a wall death -- the rig's rule).
    The verdict comes from the dump: (step top - feet) at the contact tick, and
    "did GD mount the step".
    """
    objs: list[str] = []
    x = 90.0
    xo = {1.0: 15.0 * math.tan(math.atan(1.0) / 2.0),
          0.5: 15.0 * math.tan(math.atan(0.5) / 2.0)}
    # depth is "how many px below the line the step's top surface is" = the
    # quantity that decides whether the step is mounted (boundary 3.15).
    # THE LIFT, lift = m*(15-xoff) - depth = how far the step raises the feet.
    # Whether GD launches after mounting appears to be decided by this instead
    # (the corpus' lv16 keeps riding at lift 0.22, while the rig launches at
    # lift 5.04). Sweeping depth from 3.2 to 8.5 drops lift from 5.6 to 0.3, so
    # a single run brackets both.
    for m in (1.0, 0.5):
        for depth in (2.6, 2.9, 3.05, 3.1, 3.2, 3.35, 3.6, 4.0, 4.5, 5.0,
                      5.5, 6.0, 6.5, 7.0, 7.6, 8.2):
            dz = m * (15.0 - xo[m]) - depth
            u_objs, x_next, u = ridestep_unit(x, "cube", m, dz)
            objs += u_objs
            u["x1"] = x_next
            u["depth"] = depth
            UNITS.append(u)
            x = x_next
    # Check whether it is mode-independent (only the two sides of the boundary)
    for mode in ("ball", "robot", "spider"):
        for depth in (2.6, 3.6):
            m = 1.0
            dz = m * (15.0 - xo[m]) - depth
            u_objs, x_next, u = ridestep_unit(x, mode, m, dz)
            objs += u_objs
            u["x1"] = x_next
            u["depth"] = depth
            UNITS.append(u)
            x = x_next
    objs += floor_run(0, x + 600.0)
    return header() + ";" + ";".join(objs) + ";"


def build_seam07() -> str:
    global SEAM_SPEED
    SEAM_SPEED = 1
    return build_seam()


def build_seam11() -> str:
    global SEAM_SPEED
    SEAM_SPEED = 2
    return build_seam()


BUILDERS = {"probe": build_probe, "slopes": build_slopes,
            "crush": build_crush, "sawcal": build_sawcal,
            "sawcal_wave": lambda: build_sawcal_mode("wave"),
            "sawcal_ship": lambda: build_sawcal_mode("ship"),
            "sawcal_ball": lambda: build_sawcal_mode("ball"),
            "sawcal_cube": lambda: build_sawcal_mode("cube"),
            "sawcal_robot": lambda: build_sawcal_mode("robot"),
            "sawcal_ufo": lambda: build_sawcal_mode("ufo"),
            "sawcal_swing": lambda: build_sawcal_mode("swing"),
            "sawcal_spider": lambda: build_sawcal_mode("spider"),
            "sawcal_ball_mini": lambda: build_sawcal_mode("ball", True),
            "sawcal_wave_mini": lambda: build_sawcal_mode("wave", True),
            "sawcal_ship_mini": lambda: build_sawcal_mode("ship", True),
            "sawcal_cube_mini": lambda: build_sawcal_mode("cube", True),
            "sawcalmv_wave": lambda: build_sawcal_moved("wave"),
            "sawcalmv_ship": lambda: build_sawcal_moved("ship"),
            "sawcalmv_ball": lambda: build_sawcal_moved("ball"),
            "sawcalmv_cube": lambda: build_sawcal_moved("cube"),
            "sawcalmv_robot": lambda: build_sawcal_moved("robot"),
            "sawcalmv_ufo": lambda: build_sawcal_moved("ufo"),
            "sawcalmv_swing": lambda: build_sawcal_moved("swing"),
            "sawcalmv_spider": lambda: build_sawcal_moved("spider"),
            "sawcalmv_ship_mini": lambda: build_sawcal_moved("ship", True),
            "sawcalmv_cube_mini": lambda: build_sawcal_moved("cube", True),
           "forcecal_cube": lambda: build_forcecal_mode("cube"),
           "forcedrop_cube": lambda: build_forcedrop_mode("cube"),
           "platformer": build_platformer,
           "speedcal_cube_s0": lambda: build_speedcal("cube", 0),
           "speedcal_cube_s1": lambda: build_speedcal("cube", 1),
           "speedcal_cube_s2": lambda: build_speedcal("cube", 2),
           "speedcal_cube_s3": lambda: build_speedcal("cube", 3),
           "speedcal_cube_s4": lambda: build_speedcal("cube", 4),
           "speedcal_ship_s4": lambda: build_speedcal("ship", 4),
           "speedcal_ship_s3": lambda: build_speedcal("ship", 3),
           "forcedrop_cube_mini_f10": lambda: build_forcedrop_mode("cube", True, 10),
           "forcedrop_swing_mini_f10": lambda: build_forcedrop_mode("swing", True, 10),
           "forcedrop_ball_mini_f10": lambda: build_forcedrop_mode("ball", True, 10),
           "forcedrop_ship_mini_f10": lambda: build_forcedrop_mode("ship", True, 10),
           "forcedrop_cube_f3": lambda: build_forcedrop_mode("cube", False, 3),
           "forcedrop_cube_f10": lambda: build_forcedrop_mode("cube", False, 10),
           "forcedrop_cube_f30": lambda: build_forcedrop_mode("cube", False, 30),
           "forcedrop_robot_f10": lambda: build_forcedrop_mode("robot", False, 10),
           "forcedrop_swing_f10": lambda: build_forcedrop_mode("swing", False, 10),
           "forcedrop_ship_f10": lambda: build_forcedrop_mode("ship", False, 10),
           "forcedrop_ball_f10": lambda: build_forcedrop_mode("ball", False, 10),
           "forcedrop_spider_f10": lambda: build_forcedrop_mode("spider", False, 10),
           "forcedrop_ufo_f10": lambda: build_forcedrop_mode("ufo", False, 10),
           "forcedrop_ship": lambda: build_forcedrop_mode("ship"),
           "forcedrop_ball": lambda: build_forcedrop_mode("ball"),
           "forcedrop_ufo": lambda: build_forcedrop_mode("ufo"),
           "forcedrop_wave": lambda: build_forcedrop_mode("wave"),
           "forcedrop_robot": lambda: build_forcedrop_mode("robot"),
           "forcedrop_spider": lambda: build_forcedrop_mode("spider"),
           "forcedrop_swing": lambda: build_forcedrop_mode("swing"),
           "forcecal_ship": lambda: build_forcecal_mode("ship"),
           "forcecal_ball": lambda: build_forcecal_mode("ball"),
           "forcecal_ufo": lambda: build_forcecal_mode("ufo"),
           "forcecal_wave": lambda: build_forcecal_mode("wave"),
           "forcecal_robot": lambda: build_forcecal_mode("robot"),
           "forcecal_spider": lambda: build_forcecal_mode("spider"),
           "forcecal_swing": lambda: build_forcecal_mode("swing"),
            "dualmode": build_dualmode,
            "ceilrel": build_ceilrel, "ceilrel11": build_ceilrel11,
            "ceilrel07": build_ceilrel07, "seam": build_seam,
            "seam07": build_seam07, "seam11": build_seam11,
            "ridestep": build_ridestep, "ceilhold": build_ceilhold,
            "dropair": build_dropair, "dualport": build_dualport,
            "flyramps": build_flyramps,
            "portwavebig": build_portwavebig,
            "recttop": build_recttop,
            "ramps": build_ramps, "rampseam": build_rampseam,
    "rampseamdual": build_rampseamdual,
    "ceilrampdual": build_ceilrampdual, "wavein": build_wavein,
    "ridemode": build_ridemode, "ridemodedual": build_ridemodedual,
    "ridedrop": build_ridedrop,
    "rampjump": build_rampjump,
            "ceilramp": build_ceilramp, "portrot": build_portrot,
            "portwave": build_portwave,
            "empty": build_empty, "flat": build_flat,
            "minhdr": build_minhdr, "flat90": build_flat90,
            "flatfar": build_flatfar, "orbs": build_orbs,
            "orbair": build_orbair, "orbedge": build_orbedge,
            "orbedge2": build_orbedge2, "orbedge3": build_orbedge3,
            "slopeland": build_slopeland, "slopeland3": build_slopeland3,
            "slopeland4": build_slopeland4,
            "slopeland4ball": build_slopeland4ball,
            "slopeland4fast": build_slopeland4fast,
            "shortexit": build_shortexit, "edgeland": build_edgeland,
            "slopelandball": build_slopelandball,
            "ceilpush": build_ceilpush, "ceilpushball": build_ceilpushball,
            "ceilpush8": build_ceilpush8}


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("what", choices=sorted(BUILDERS))
    ap.add_argument("--out", required=True)
    ap.add_argument("--xmap", default=None,
                    help="a measured dump.csv. Press ticks are taken from its "
                         "x(t) -- pass a dump of one run of this same rig")
    ap.add_argument("--arc", default=None,
                    help="the dump.csv of slopeland pass1. The pad arc is "
                         "measured from it to place the slopes; without it the "
                         "rig has pads only")
    a = ap.parse_args()
    if a.xmap:
        load_xmap(Path(a.xmap))
        print(f"xmap: {len(XMAP)} ticks (ends at x={XMAP[-1][1]:.0f})")
    if a.arc:
        load_arc(Path(a.arc))
    s = BUILDERS[a.what]()
    p = Path(a.out)
    p.parent.mkdir(parents=True, exist_ok=True)
    # Write it raw (compression is left to GD's ZipUtils on the MOD side).
    p.write_text(s, encoding="utf-8", newline="")
    n = s.count(";") - 1
    print(f"{a.what}: {n} objects, {len(s)} chars -> {p}")
    if UNITS:
        up = p.with_suffix(".units.json")
        up.write_text(json.dumps(UNITS, indent=1), encoding="utf-8")
        print(f"  unit table, {len(UNITS)} entries -> {up}")
    if PLAN:
        pp = p.with_suffix(".plan.txt")
        pp.write_text("".join(f"input={t},{d}\n" for t, d in sorted(PLAN)),
                      encoding="utf-8", newline="")
        print(f"  {len(PLAN)} presses -> {pp}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
