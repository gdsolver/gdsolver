"""Build a TYPED member table for PlayerObject / GameObject from the bindings (.bro).

Why the types matter: psnap's whitelist guarantees safety by "never copying a
single pointer". With only names and sizes we would have to discard every
8-byte member wholesale, which also drops GENUINE PHYSICS QUANTITIES such as
`double m_yVelocityBeforeSlope` (measured: the whitelist shrank to 756/3,144
bytes and the solutions the search produced stopped reproducing).

Output `src/po_members.inc`:
    PO_SCALAR(name)   POD that cannot contain a pointer; psnap may copy it
    PO_OPAQUE(name)   pointer / container / unknown; must not be copied
    GO_SCALAR / GO_OPAQUE work the same way

ALWAYS ERR TOWARDS OPAQUE FOR AN UNKNOWN TYPE. When in doubt, not copying is
safer (a missed copy shows up in verification as a physics divergence, but
copying a pointer crashes with heap corruption).

    python scripts/gen_member_table.py [--bro <path>] [--report]
"""

from __future__ import annotations

import argparse
import re
from pathlib import Path

BRO = Path(r"C:\GD\build\_deps\bindings-src\bindings\2.2081\GeometryDash.bro")
OUT = Path(r"C:\GD\src\po_members.inc")

# Only types confirmed unable to contain a pointer. Anything not here is OPAQUE.
SCALAR_TYPES = {
    "bool", "char", "signed char", "unsigned char", "short", "unsigned short",
    "int", "unsigned int", "unsigned", "long", "unsigned long",
    "long long", "unsigned long long", "float", "double",
    "int8_t", "uint8_t", "int16_t", "uint16_t", "int32_t", "uint32_t",
    "int64_t", "uint64_t", "size_t",
    # cocos PODs (made of nothing but floats)
    "cocos2d::CCPoint", "cocos2d::CCSize", "cocos2d::CCRect",
    "cocos2d::ccColor3B", "cocos2d::ccColor4B", "cocos2d::ccColor4F",
    "cocos2d::CCAffineTransform",
}
# An enum is scalar since it is int underneath. Pick up the ones the bro emits
# as enum class.
ENUM_RE = re.compile(r"^(?:enum\s+)?(?:class\s+)?[A-Z]\w*(?:Type|Mode|State|Key)$")

FIELD_RE = re.compile(r"^\s{4}([A-Za-z_][\w:<>,\s\*&]*?)\s+(m_\w+)\s*;\s*$")


def parse_class(lines: list[str], name: str) -> list[tuple[str, str]]:
    start = None
    for i, l in enumerate(lines):
        if l.startswith(f"class {name} ") or l.rstrip() == f"class {name} {{":
            start = i
            break
    if start is None:
        raise SystemExit(f"class {name} not found in the bindings")
    out = []
    for l in lines[start:]:
        if l.startswith("}"):
            break
        if "=" in l or "(" in l:      # skip functions and address-annotated decls
            continue
        m = FIELD_RE.match(l)
        if m:
            out.append((" ".join(m.group(1).split()), m.group(2)))
    return out


# A name that appears in the .bro as `class X` is not an enum. Guessing enums
# from the shape of the name alone MISCLASSIFIES STRUCTS LIKE GJGameState AS
# ENUMS (we actually did this, and treated a struct holding 20+ CCPoints as a
# scalar).
_CLASS_NAMES: set[str] = set()


def load_class_names(lines: list[str]) -> None:
    for l in lines:
        m = re.match(r"^class\s+([A-Za-z_]\w*)", l)
        if m:
            _CLASS_NAMES.add(m.group(1))


def is_scalar(ty: str) -> bool:
    if "*" in ty or "&" in ty or "<" in ty:
        return False
    if ty.startswith(("gd::", "std::")):
        return False
    if ty in SCALAR_TYPES:
        return True
    short = ty.split("::")[-1]
    if short in _CLASS_NAMES:
        return False                      # a class/struct, not an enum
    return bool(ENUM_RE.match(short))


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--bro", default=str(BRO))
    ap.add_argument("--out", default=str(OUT))
    ap.add_argument("--report", action="store_true",
                    help="print the type breakdown only, generate nothing")
    a = ap.parse_args()

    lines = Path(a.bro).read_text(encoding="utf-8", errors="replace").splitlines()
    load_class_names(lines)
    po = parse_class(lines, "PlayerObject")
    go = parse_class(lines, "GameObject")

    if a.report:
        seen: dict[str, int] = {}
        for ty, _ in po + go:
            seen[ty] = seen.get(ty, 0) + 1
        for ty, n in sorted(seen.items(), key=lambda kv: -kv[1]):
            print(f"  {'SCALAR' if is_scalar(ty) else 'opaque':>6}  {n:>3}  {ty}")
        return 0

    # GJBaseGameLayer is the state OUTSIDE m_objects (timers, counters, camera,
    # the group/trigger ledger). Once the player and the objects are cleared as
    # psnap suspects, this is what remains. PlayLayer inherits from it, so the
    # offsetof values apply unchanged to a PlayLayer instance.
    gb = parse_class(lines, "GJBaseGameLayer")
    # GJEffectManager is the live counterpart of what CheckpointObject saves
    # wholesale as `EffectManagerState`. It holds the trigger queue and the
    # group commands still in progress.
    em = parse_class(lines, "GJEffectManager")

    rows = []
    ns = no = 0
    for pfx, fields in (("PO", po), ("GO", go), ("GB", gb), ("EM", em)):
        for ty, nm in fields:
            if is_scalar(ty):
                rows.append(f"{pfx}_SCALAR({nm})")
                ns += 1
            else:
                rows.append(f"{pfx}_OPAQUE({nm})")
                no += 1
    hdr = ["// Generated by scripts/gen_member_table.py -- do not edit by hand",
           "// SCALAR = POD that cannot hold a pointer (psnap may copy it)",
           "// OPAQUE = pointer, container or unknown (must not be copied)",
           f"// PlayerObject {len(po)} / GameObject {len(go)} / "
           f"GJBaseGameLayer {len(gb)} / GJEffectManager {len(em)} "
           f"-> scalar {ns} / opaque {no}"]
    Path(a.out).write_text("\n".join(hdr + rows) + "\n", encoding="utf-8")
    print(f"{a.out}: {ns} scalar / {no} opaque")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
