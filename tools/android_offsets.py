#!/usr/bin/env python
"""Re-check src/mod/gd_offsets.hpp: every raw offset, on Windows and android64.

    python tools/android_offsets.py [--bindings DIR] [--so libcocos2dcpp.so] [--all]

Three checks, all read-only:

1. Layout model. The bindings (.bro) list each class's members in declaration order, the same
   on every platform; what differs is the size of the STL types (MSVC's on Windows, the GNU
   STL clones Geode uses for GD on Android) and the Itanium ABI's reuse of a base's tail
   padding. The model lays each class out both ways and must reproduce BOTH columns of every
   GDOFF(win, android64) for the member src/mod/gd_offsets_check.cpp ties it to. A mismatch
   is a failure (exit 1).
2. The binary (--so, the android64 libcocos2dcpp.so, never committed). For each constant it
   counts the loads/stores of the Android value, of the member's width, inside the functions
   of the classes that own the member. Zero is reported, not failed: an array read through an
   index register, for example, has no immediate to find.
3. The three LCG states (kSeed*Rva): each must be the global an inlined x * 214013 + 2531011
   in the function SEED_SITES names reads and writes.

The class starts the model cannot derive (the cocos2d base sizes) are fixed in ROOTS below,
with where each was measured. Needs capstone and lief for --so (pip install capstone lief).
"""
from __future__ import annotations

import argparse
import bisect
import hashlib
import re
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
HEADER = REPO / "src" / "mod" / "gd_offsets.hpp"
CHECK = REPO / "src" / "mod" / "gd_offsets_check.cpp"
DEFAULT_BINDINGS = REPO / "build" / "_deps" / "bindings-src" / "bindings"
GD_VERSION = "2.2081"
KNOWN_SHA256 = "dda3752ab3a912fd2293561e157ecc98eca799056abd9223a331c1c514175df7"

# Where each modelled class's own members start. Windows: the literals the mod used before
# the header existed (m_gameState at +0x1a8 is getMaxPortalY's camera-zoom divisor; GameObject
# from PlayerObject's m_slopeStartTime at +0x598). android64: measured -- m_maxGameplayY is
# stored at +0x3690 by updateMaxGameplayY, and removeObjectFromSection / addToSection keep the
# section indices at +0x26c..+0x278. Everything else, PlayerObject and the GameObject
# subclasses included, is derived from these.
ROOTS = {
    "GJBaseGameLayer": {"win": 0x1A0, "a64": 0x1A0},
    "GameObject": {"win": 0x270, "a64": 0x26C},
    "EnterEffectInstance": {"win": 0, "a64": 0},
    "GJGameState": {"win": 0, "a64": 0},
}
# Classes the check file names but the model has no root for: reported, not modelled.
UNMODELLED = {"LevelInfoLayer", "LevelPage", "EditLevelLayer", "GJGameLevel", "CheckpointObject"}

# ---------------------------------------------------------------------------- bindings model


class Bindings:
    PLAT_WORDS = ("win", "android", "android32", "android64", "ios", "mac", "imac", "m1")

    def __init__(self, bindings: Path):
        bro = bindings / GD_VERSION / "GeometryDash.bro"
        enums = bindings / "include" / "Geode" / "Enums.hpp"
        self.lines = bro.read_text(encoding="utf-8").split("\n")
        self.enum_under = {}
        for m in re.finditer(r"enum (?:class |struct )?(\w+)\s*(?::\s*([\w: ]+?))?\s*\{",
                             enums.read_text(encoding="utf-8")):
            self.enum_under[m.group(1)] = (m.group(2) or "int").strip()
        self.classes: dict[str, tuple[list[str], list[tuple]]] = {}
        self._parse()
        self._cache: dict = {}

    def _parse(self) -> None:
        plat_rx = r"(?:%s)" % "|".join(self.PLAT_WORDS)
        block_rx = re.compile(r"^\s+(%s(?:\s*,\s*%s)*)\s*\{\s*$" % (plat_rx, plat_rx))
        i = 0
        L = self.lines
        while i < len(L):
            m = re.match(r"^(?:\[\[.*\]\]\s*)?class ([\w:]+)\s*(?::\s*([^{]+))?\{", L[i])
            if not m:
                i += 1
                continue
            name = m.group(1)
            bases = [b.strip() for b in (m.group(2) or "").split(",") if b.strip()]
            mem, plats = [], None
            i += 1
            while not L[i].startswith("}"):
                s = L[i].split("//")[0].rstrip()
                i += 1
                bm = block_rx.match(s)
                if bm:
                    plats = set()
                    for w in (x.strip() for x in bm.group(1).split(",")):
                        if w == "win":
                            plats.add("win")
                        if w in ("android", "android64"):
                            plats.add("a64")
                    continue
                if plats is not None and s.strip() == "}":
                    plats = None
                    continue
                pm = re.match(r"^\s+PAD\s*=\s*(.*);", s)
                if pm:
                    d = dict(re.findall(r"(\w+)\s+(0x[0-9a-fA-F]+)", pm.group(1)))
                    pad = {"win": int(d.get("win", "0"), 16), "a64": int(d.get("android64", "0"), 16)}
                    mem.append(("PAD", "__pad", None, pad, plats))
                    continue
                if "(" in s:
                    continue
                mm = re.match(r"^\s+(?!static\b|virtual\b|inline\b|friend\b|using\b)(.+?)\s+(\w+)"
                              r"(?:\[(\w+)\])?\s*;\s*$", s)
                if mm:
                    mem.append((mm.group(1).strip(), mm.group(2), mm.group(3), None, plats))
            self.classes[name] = (bases, mem)
            i += 1

    # sizes -----------------------------------------------------------------
    PRIM = {"bool": 1, "char": 1, "unsigned char": 1, "uint8_t": 1, "int8_t": 1, "signed char": 1,
            "short": 2, "unsigned short": 2, "int16_t": 2, "uint16_t": 2, "int": 4,
            "unsigned int": 4, "unsigned": 4, "uint32_t": 4, "int32_t": 4, "float": 4,
            "double": 8, "long long": 8, "unsigned long long": 8, "int64_t": 8, "uint64_t": 8,
            "size_t": 8, "uintptr_t": 8, "intptr_t": 8, "time_t": 8}
    COCOS = {"CCPoint": (8, 4), "CCSize": (8, 4), "CCRect": (16, 4), "ccColor3B": (3, 1),
             "ccColor4B": (4, 1), "ccColor4F": (16, 4), "ccBlendFunc": (8, 4),
             "ccHSVValue": (16, 4), "CCAffineTransform": (24, 4)}
    unknown: set = set()

    @staticmethod
    def _args(s: str) -> list[str]:
        out, depth, cur = [], 0, ""
        for ch in s:
            depth += (ch == "<") - (ch == ">")
            if ch == "," and depth == 0:
                out.append(cur.strip())
                cur = ""
            else:
                cur += ch
        return out + ([cur.strip()] if cur.strip() else [])

    def size_align(self, t: str, plat: str) -> tuple[int, int]:
        t = re.sub(r"\bconst\b", "", t).strip()
        if t.endswith(("*", "&")):
            return 8, 8
        if t in self.PRIM:
            n = self.PRIM[t]
            return n, n
        if t in ("long", "unsigned long"):
            return (4, 4) if plat == "win" else (8, 8)
        if "<" not in t and t.split("::")[-1] in self.COCOS:
            return self.COCOS[t.split("::")[-1]]
        m = re.match(r"^(gd|std)::(\w+)<(.*)>$", t)
        if m:
            kind, args = m.group(2), self._args(m.group(3))
            if kind == "vector":
                if args[0] == "bool":
                    return (32, 8) if plat == "win" else (40, 8)
                return 24, 8
            if kind in ("map", "set"):
                return (16, 8) if plat == "win" else (48, 8)
            if kind in ("unordered_map", "unordered_set"):
                return (64, 8) if plat == "win" else (56, 8)
            if kind == "array":
                s, a = self.size_align(args[0], plat)
                return s * int(args[1], 0), a
            if kind in ("pair", "tuple"):
                return self._struct([(x, None) for x in args], plat)[:2]
            if kind == "deque":
                return (40, 8) if plat == "win" else (80, 8)
            if kind == "function":
                return (64, 8) if plat == "win" else (32, 8)
        if t in ("gd::string", "std::string"):
            return (32, 8) if plat == "win" else (8, 8)
        if re.match(r"^geode::SeedValue\w+$", t):
            return 4 * len(t.split("SeedValue")[1]), 4
        tn = t.split("::")[-1]
        if tn in self.enum_under:
            return self.size_align(self.enum_under[tn], plat)
        if tn in self.classes and not self.classes[tn][0]:
            s, a, _ = self.layout(tn, plat)
            return s, a
        self.unknown.add(t)
        return 4, 4

    def _struct(self, fields, plat):
        off, al, out = 0, 1, []
        for t, n in fields:
            s, a = self.size_align(t, plat)
            off = (off + a - 1) // a * a
            out.append((n, off, s, t))
            off += s
            al = max(al, a)
        return (off + al - 1) // al * al, al, out

    def layout(self, name: str, plat: str, start: int = 0):
        _, mem = self.classes[name]
        off, al, out = start, 1, []
        for t, n, cnt, pad, pl in mem:
            if pl is not None and plat not in pl:
                continue
            if t == "PAD":
                out.append(("__pad", off, pad[plat], "PAD"))
                off += pad[plat]
                continue
            s, a = self.size_align(t, plat)
            if cnt:
                s *= int(cnt, 0)
            off = (off + a - 1) // a * a
            out.append((n, off, s, t))
            off += s
            al = max(al, a)
        return (off + al - 1) // al * al, al, out

    # absolute layouts --------------------------------------------------------
    def own_start(self, cls: str, plat: str) -> int:
        if cls in ROOTS:
            return ROOTS[cls][plat]
        bases = self.classes[cls][0]
        size, _, _, dsize = self.absolute(bases[0], plat)
        # Itanium places a derived class's members in its base's tail padding; MSVC does not.
        end = dsize if plat == "a64" else size
        if len(bases) > 1:  # secondary bases here are delegates: one vtable pointer each
            end = (end + 7) // 8 * 8 + 8 * (len(bases) - 1)
        return end

    def absolute(self, cls: str, plat: str):
        key = (cls, plat)
        if key not in self._cache:
            st = self.own_start(cls, plat)
            _, al, lay = self.layout(cls, plat, st)
            inherited = [] if cls in ROOTS else self.absolute(self.classes[cls][0][0], plat)[2]
            d = max((o + s for _, o, s, _ in lay), default=st)
            self._cache[key] = ((max(d, st) + 7) // 8 * 8, al, inherited + lay, d)
        return self._cache[key]

    def member(self, cls: str, path: str, plat: str) -> tuple[int, str]:
        parts = path.split(".")
        for n, o, s, t in self.absolute(cls, plat)[2]:
            if n == parts[0]:
                if len(parts) == 1:
                    return o, t
                sub = t.split("::")[-1]
                for n2, o2, s2, t2 in self.layout(sub, plat)[2]:
                    if n2 == parts[1]:
                        return o + o2, t2
        raise KeyError(f"{cls}::{path} is not in the bindings")


# ---------------------------------------------------------------------------- header + checks


def read_constants() -> dict[str, tuple[int, int]]:
    text = HEADER.read_text(encoding="utf-8")
    return {m.group(1): (int(m.group(2), 16), int(m.group(3), 16))
            for m in re.finditer(r"constexpr std::\w+ (k\w+) = GDOFF\((0x[0-9a-fA-F]+), "
                                 r"(0x[0-9a-fA-F]+)\)", text)}


def read_asserts() -> list[tuple[str, int, str, str, int]]:
    """(constant, +n, class, member path, extra) for every static_assert in the check file."""
    out = []
    text = CHECK.read_text(encoding="utf-8")
    for m in re.finditer(r"static_assert\((k\w+)(?: \+ (\d))?\s*==\s*(.*?)\);", text, re.S):
        expr = " ".join(m.group(3).split())
        plus = int(m.group(2) or 0)
        sm = re.fullmatch(r"STATE\((\w+)\)( \+ offsetof\(CCPoint, y\))?", expr)
        if sm:
            out.append((m.group(1), plus, "GJBaseGameLayer", "m_gameState." + sm.group(1),
                        4 if sm.group(2) else 0))
            continue
        om = re.fullmatch(r"offsetof\((\w+), (\w+)\)", expr)
        if not om:
            raise SystemExit(f"cannot read the assert for {m.group(1)}: {expr}")
        out.append((m.group(1), plus, om.group(1), om.group(2), 0))
    return out


# ---------------------------------------------------------------------------- the binary


class Binary:
    def __init__(self, path: Path):
        import capstone
        import lief

        self.raw = path.read_bytes()
        self.sha256 = hashlib.sha256(self.raw).hexdigest()
        self.elf = lief.parse(str(path))
        text = self.elf.get_section(".text")
        self.text = bytes(text.content)
        self.tbase = text.virtual_address
        self.syms: dict[int, str] = {}
        for s in self.elf.dynamic_symbols:
            if s.is_function and s.value:
                self.syms[s.value] = s.demangled_name or s.name
        self.addrs = sorted(self.syms)
        self.cs = capstone.Cs(capstone.CS_ARCH_ARM64, capstone.CS_MODE_ARM)
        self._acc: dict[int, list] = {}

    def size(self, a: int) -> int:
        i = bisect.bisect_right(self.addrs, a)
        return (self.addrs[i] if i < len(self.addrs) else a + 0x4000) - a

    def dis(self, a: int):
        off = a - self.tbase
        return list(self.cs.disasm(self.text[off:off + self.size(a)], a))

    def functions(self, prefixes: tuple[str, ...]):
        return [a for a in self.addrs if self.syms[a].startswith(prefixes)]

    def accesses(self, a: int):
        if a not in self._acc:
            acc = []
            for i in self.dis(a):
                m = re.search(r"\[(x\d+), #(0x[0-9a-f]+)\]", i.op_str)
                if m and i.mnemonic.startswith(("ldr", "str", "ldp", "stp")) \
                        and not i.op_str.startswith(("x29", "x30")):
                    acc.append((int(m.group(2), 16), i.mnemonic, i.op_str.split(",")[0].strip()))
            self._acc[a] = acc
        return self._acc[a]

    def lcg_globals(self, fn_prefix: str) -> set[int]:
        """Globals read or written near a `mov #0x43fd` (the LCG multiplier's low half)."""
        out: set[int] = set()
        for a in self.functions((fn_prefix,)):
            ins = self.dis(a)
            pages: dict[str, int] = {}
            refs = []
            for i in ins:
                ops = [x.strip() for x in i.op_str.split(",")]
                if i.mnemonic == "adrp":
                    pages[ops[0]] = int(ops[1].lstrip("#"), 16)
                    continue
                m = re.match(r"(\w+), \[(x\d+), #(0x[0-9a-f]+)\]", i.op_str)
                if m and m.group(2) in pages:
                    refs.append((i.address, pages[m.group(2)] + int(m.group(3), 16)))
                    continue
                m = re.match(r"(x\d+), (x\d+), #(0x[0-9a-f]+)$", i.op_str)
                if i.mnemonic == "add" and m and m.group(2) in pages:
                    pages[m.group(1)] = pages[m.group(2)] + int(m.group(3), 16)
            for i in ins:
                if i.mnemonic == "mov" and i.op_str.endswith("#0x43fd"):
                    out |= {g for at, g in refs if abs(at - i.address) < 0x40}
        return out


OWNERS = {
    "GJBaseGameLayer": ("GJBaseGameLayer::", "PlayLayer::"),
    "PlayerObject": ("PlayerObject::",),
    "GameObject": ("GameObject::", "GJBaseGameLayer::", "PlayLayer::", "PlayerObject::"),
    "RingObject": ("RingObject::", "PlayerObject::"),
    "EnterEffectObject": ("EnterEffectObject::", "GJBaseGameLayer::"),
    "EnterEffectInstance": ("EnterEffectInstance::", "GJBaseGameLayer::"),
    "CheckpointObject": ("CheckpointObject::", "PlayLayer::"),
    "LevelInfoLayer": ("LevelInfoLayer::",),
    "LevelPage": ("LevelPage::",),
    "EditLevelLayer": ("EditLevelLayer::",),
    "GJGameLevel": ("LevelPage::", "LevelInfoLayer::", "GJGameLevel::"),
}


def width_ok(t: str, mnemonic: str, reg: str) -> bool:
    if t in ("bool", "unsigned char", "char"):
        return mnemonic in ("ldrb", "strb", "ldrsb")
    if t == "short":
        return mnemonic in ("ldrh", "strh", "ldrsh")
    if t in ("int", "unsigned int") or t.endswith("Type"):
        return reg.startswith("w") or mnemonic == "ldrsw"
    if t == "float":
        return reg.startswith(("s", "w"))
    if t in ("double", "uint64_t"):
        return reg.startswith(("d", "x"))
    return reg.startswith(("x", "q", "d", "s", "w"))


# The functions whose inlined LCG reads and writes each seed: the trigger seed is
# GameToolbox::fast_rand's state, the variance-index seed the one resetObject draws from, the
# variance-table seed the one GJBaseGameLayer::init fills m_varianceValues from.
SEED_SITES = {
    "kSeedTriggerRva": "GameToolbox::fast_rand()",
    "kSeedVarIndexRva": "GameObject::resetObject()",
    "kSeedVarTableRva": "GJBaseGameLayer::init()",
}


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--bindings", type=Path, default=DEFAULT_BINDINGS,
                    help="the bindings/ directory of geode-sdk/bindings (default: the build's)")
    ap.add_argument("--so", type=Path, help="android64 libcocos2dcpp.so to check against")
    ap.add_argument("--all", action="store_true", help="print every row, not only problems")
    args = ap.parse_args()

    model = Bindings(args.bindings)
    consts = read_constants()
    asserts = read_asserts()
    binary = Binary(args.so) if args.so else None
    if binary:
        note = "" if binary.sha256 == KNOWN_SHA256 else "  (NOT the binary the header was measured on)"
        print(f"binary sha256 {binary.sha256}{note}")

    failures = unseen = 0
    for name, plus, cls, path, extra in asserts:
        win, a64 = consts[name]
        win, a64 = win + plus, a64 + plus
        row = f"{name}{'+%d' % plus if plus else ''}: {cls}::{path} win {win:#x} a64 {a64:#x}"
        problems = []
        t = "?"
        if cls not in UNMODELLED:
            mw, t = model.member(cls, path, "win")
            ma, _ = model.member(cls, path, "a64")
            mw, ma = mw + extra, ma + extra
            if (mw, ma) != (win, a64):
                problems.append(f"model win {mw:#x} a64 {ma:#x}")
        hits = None
        if binary and cls in OWNERS:
            hits = 0
            for f in binary.functions(OWNERS[cls]):
                for o, mn, reg in binary.accesses(f):
                    if o == a64 and (extra or width_ok(t, mn, reg) or t == "?"):
                        hits += 1
            if hits == 0:
                unseen += 1
        if problems:
            failures += 1
        if problems or args.all or hits == 0:
            tag = "FAIL" if problems else ("unseen" if hits == 0 else "ok")
            h = "" if hits is None else f"  hits {hits}"
            print(f"{tag:6s} {row}{h}  {'; '.join(problems)}")
    if binary:
        for name, fn in SEED_SITES.items():
            a64 = consts[name][1]
            found = binary.lcg_globals(fn)
            ok = a64 in found
            failures += not ok
            if not ok or args.all:
                print(f"{'ok' if ok else 'FAIL':6s} {name}: {fn} LCG globals "
                      f"{sorted(hex(g) for g in found)}, header {a64:#x}")
    if model.unknown:
        print("types the model could not size:", sorted(model.unknown))
    print(f"{len(asserts)} member offsets, {len(SEED_SITES) if binary else 0} seeds: "
          f"{failures} failed" + (f", {unseen} without a matching access in the binary"
                                  if binary else " (no --so: binary not checked)"))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
