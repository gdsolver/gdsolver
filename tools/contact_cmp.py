"""contact_cmp.py <ctrace.csv (game)> <model trace.csv (--tracecontact)> [--shift K] [--list N] -- the game's contact record against the model's.

The game side (mod cfg ctrace=1) logs, per tick at the dump row's point, what the collision pass left in player 1: fl / ce / le / ri (the unique IDs of the
solid stood on, bumped with the head, touched on the left / right; -1 = none), the contact lines flY / ceY, the ramp (slope, onSlope), the ground bytes
G / G2 / BOOST and snap (m_objectSnappedTo).
The model side (leveldp --tracecontact, beside the clamp columns the trace always has) logs snapuid (State::snapObj), slopeuid (State::slopeUidNow),
gunid (State::groundUid) and grounded / onslope, plus clamp / clampuid: the reason and the uid of the last tick's vy := 0 clamp.
The model has no per-face contact record of its own; the comparison reads it as:
    floor   game: fl >= 0 and (G or G2)       model: grounded, uid = snapuid             (a solid under it; compared only on ticks the game has one)
    snap    game: snap (m_objectSnappedTo)    model: snapuid                             (the last solid stood on, kept on both sides)
    ceiling game: ce >= 0                     model: clamp names a ceiling / head / bottom reason, uid = clampuid
    ramp    game: onSlope                     model: onslope, uid = slopeuid
    ground  game: G                           model: grounded                            (cube-family modes only: State::grounded is "cube only")
and prints, per class, the number of ticks compared, the number that differ in presence and, where both have one, the number that differ in uid; then the
first --list runs of ticks that differ.  Measurement only; the tick shift defaults to 0 (the dump row's tick on both sides).
--until T compares only the ticks before T: once the model's position has left the game's the two are not in the same place and a contact
difference there is a consequence, not a cause."""
import csv
import sys
from collections import defaultdict

CEIL_WORDS = ("ceil", "head", "bottom", "under", "flyhead")
FOOT_MODES = ("0", "2", "5", "6")   # cube, ball, robot, spider (State::mode numbering of the replay trace)


def load_game(path):
    rows = [r for r in csv.DictReader(open(path, newline="")) if r.get("snap") not in (None, "")]
    if not rows:
        return {}
    last = max(int(r["attempt"]) for r in rows)
    return {int(r["tick"]): r for r in rows if int(r["attempt"]) == last}


def load_model(path):
    out = {}
    with open(path, newline="") as f:
        for r in csv.DictReader(f):
            out[int(r["tick"])] = r
    return out


def ceil_reason(why):
    return any(w in why for w in CEIL_WORDS)


def classes(g, m):
    """-> {class: (game uid or None, model uid or None)}; None = no contact of that class on that side."""
    out = {}
    # support: the game has a solid under it (fl >= 0 with a ground byte up); the model must be grounded on the same uid.  Only the game's side
    # decides whether a comparison is made: fl is -1 on the level's floor plane and m_objectSnappedTo / State::snapObj stay set after the solid
    # is left, so a presence test the other way round would count the plane as a missing solid.
    fl = int(g["fl"])
    # A ramp the player rides is also written as its floor (collidedWithSlopeInternal calls updateCollide(1, ramp)); that is the ramp class's.
    sup = fl if (fl >= 0 and (int(g["G"]) or int(g["G2"])) and fl not in (int(g["slope"]), int(g["slope2"]))) else None
    # The model's candidates for "the solid it is on this tick": the uid of this tick's landing clamp when it names a floor (State::snapObj is
    # only the last solid stood on and is not updated by every mode), and snapuid while grounded.
    cands = []
    cw, cu0 = m.get("clamp", "-"), int(m.get("clampuid", -1))
    if cw != "-" and not ceil_reason(cw) and cu0 >= 0:
        cands.append(cu0)
    if int(m["grounded"]) and int(m["snapuid"]) >= 0:
        cands.append(int(m["snapuid"]))
    msup = (sup if sup in cands else (cands[0] if cands else None)) if sup is not None else None
    out["floor"] = (sup, msup) if sup is not None else (None, None)
    # snap: both sides keep the last solid stood on
    gs, ms = int(g["snap"]), int(m["snapuid"])
    out["snap"] = (gs if gs >= 0 else None, ms if ms >= 0 else None)
    ce = int(g["ce"])
    why = m.get("clamp", "-")
    cu = int(m.get("clampuid", -1))
    out["ceil"] = (ce if ce >= 0 else None,
                   cu if (why != "-" and ceil_reason(why) and cu >= 0) else None)
    sl = int(g["slope"]) if int(g["onSlope"]) else None
    out["ramp"] = (sl, int(m["slopeuid"]) if int(m["onslope"]) and int(m["slopeuid"]) >= 0 else None)
    return out


def main():
    game = load_game(sys.argv[1])
    model = load_model(sys.argv[2])
    shift, lim, until = 0, 40, None
    a = sys.argv[3:]
    for i, x in enumerate(a):
        if x == "--shift":
            shift = int(a[i + 1])
        if x == "--list":
            lim = int(a[i + 1])
        if x == "--until":   # compare only ticks below this one (the model's first position divergence: after it the two are not in the same place)
            until = int(a[i + 1])
    n = 0
    stat = defaultdict(lambda: [0, 0, 0, 0])   # class -> [compared with a contact on either side, presence differs, both present, uid differs]
    bad = defaultdict(list)
    gbad = 0
    gcmp = 0
    for t, g in sorted(game.items()):
        if until is not None and t >= until:
            break
        m = model.get(t + shift)
        if m is None:
            continue
        n += 1
        if m["mode"] in FOOT_MODES:
            gcmp += 1
            if int(g["G"]) != int(m["grounded"]):
                gbad += 1
                bad["G"].append((t, "game G=%s model grounded=%s mode=%s" % (g["G"], m["grounded"], m["mode"])))
        for k, (gu, mu) in classes(g, m).items():
            if gu is None and mu is None:
                continue
            s = stat[k]
            s[0] += 1
            if (gu is None) != (mu is None):
                s[1] += 1
                bad[k].append((t, "game %s model %s  (clamp=%s clampuid=%s mode=%s)" % (gu, mu, m.get("clamp"), m.get("clampuid"), m["mode"])))
            else:
                s[2] += 1
                if gu != mu:
                    s[3] += 1
                    bad[k].append((t, "uid game %s model %s  (clamp=%s mode=%s)" % (gu, mu, m.get("clamp"), m["mode"])))
    print("compared %d ticks (shift %+d)" % (n, shift))
    print("G (cube family only): compared %d  differs %d" % (gcmp, gbad))
    for k in ("floor", "snap", "ceil", "ramp"):
        s = stat[k]
        print("%-5s contact on either side %d  presence differs %d  both %d  uid differs %d" % (k, s[0], s[1], s[2], s[3]))
    for k in ("G", "floor", "snap", "ceil", "ramp"):
        runs = []
        for t, txt in bad[k]:
            if runs and t == runs[-1][1] + 1 and txt == runs[-1][2]:
                runs[-1][1] = t
            else:
                runs.append([t, t, txt])
        if runs:
            print("-- %s: %d runs" % (k, len(runs)))
            for r in runs[:lim]:
                print("  t=%d..%d  %s" % (r[0], r[1], r[2]))


if __name__ == "__main__":
    main()
