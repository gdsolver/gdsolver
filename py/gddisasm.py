# -*- coding: utf-8 -*-
u"""Disassemble GD's exe at the win addresses from the bindings.

ASK THE BINARY FIRST, BEFORE REWRITING THE MODEL ON A GUESS
([[gd-disasm-first]]). Sweeps and injections are for corroboration; reversing
that order just repeats generalisations from a single data point.

    python py/gddisasm.py --sym GJBaseGameLayer::collisionCheckObjects --n 2000
    python py/gddisasm.py 0x2158f9 --n 40
    python py/gddisasm.py --xref PlayerObject::propellPlayer   # callers
    python py/gddisasm.py --float 0x622bac                     # rodata constant

Traps:
- THE ALIGNMENT POINT MUST BE THE HEAD OF A FUNCTION. Starting mid-function
  reads displacement bytes as instructions and produces plausible-looking fake
  results (docs/findings.md). `--sym` always points at the head, so start from
  the head and walk down to the address you want.
- THE INSTALLED GD IS 2.2081 ([[gd-binary-is-2-2081]]). Looking things up in
  the 2.208 bindings reads a different function. The default is pinned here.
- A rip-relative constant is "the address of the NEXT instruction + disp".
  --float exists to read those.
"""
from __future__ import annotations

import argparse
import re
import struct
from pathlib import Path

try:
    import capstone
except ImportError:                                            # pragma: no cover
    raise SystemExit("capstone is required: python -m pip install capstone")

# Read a worker's copy (identical to the real one, and readable even while held).
# [2026-08-21] worker-99 is sometimes empty (no exe deployed), so SEARCH THE
# WORKERS IN ORDER FOR ONE THAT EXISTS. They are all the same binary, so the
# number does not matter.
def _worker_exe() -> Path:
    for w in (99, 98, 97, 96, 95, 94, 93, 92, 91, 90):
        p = Path(r"C:\GD-workers\worker-%d\GeometryDash-worker-%d.exe" % (w, w))
        if p.exists():
            return p
    return Path(r"C:\GD-workers\worker-99\GeometryDash-worker-99.exe")


EXE = _worker_exe()
BRO = Path(r"C:\GD\build\_deps\bindings-src\bindings\2.2081\GeometryDash.bro")


class PE:
    """A bare-minimum PE32+ reader (RVA -> file offset)."""

    def __init__(self, path: Path):
        self.buf = path.read_bytes()
        e_lfanew = struct.unpack_from("<I", self.buf, 0x3C)[0]
        if self.buf[e_lfanew:e_lfanew + 4] != b"PE\0\0":
            raise ValueError(f"no PE header: {path}")
        coff = e_lfanew + 4
        nsec, = struct.unpack_from("<H", self.buf, coff + 2)
        opt_size, = struct.unpack_from("<H", self.buf, coff + 16)
        opt = coff + 20
        magic, = struct.unpack_from("<H", self.buf, opt)
        if magic != 0x20B:
            raise ValueError("this assumes PE32+ (x64)")
        self.image_base, = struct.unpack_from("<Q", self.buf, opt + 24)
        sec = opt + opt_size
        self.sections = []
        for i in range(nsec):
            o = sec + i * 40
            name = self.buf[o:o + 8].rstrip(b"\0").decode("ascii", "replace")
            vsize, vaddr, rawsize, rawptr = struct.unpack_from(
                "<IIII", self.buf, o + 8)
            self.sections.append((name, vaddr, vsize, rawptr, rawsize))

    def off(self, rva: int) -> int | None:
        for _, vaddr, vsize, rawptr, rawsize in self.sections:
            if vaddr <= rva < vaddr + max(vsize, rawsize):
                d = rva - vaddr
                return rawptr + d if d < rawsize else None
        return None

    def text(self) -> tuple[int, bytes]:
        for name, vaddr, _vsize, rawptr, rawsize in self.sections:
            if name == ".text":
                return vaddr, self.buf[rawptr:rawptr + rawsize]
        raise KeyError("no .text section")


def sym_addr(name: str, bro: Path = BRO) -> int:
    u"""Turn `Class::method` into the win address from the bindings."""
    cls, _, fn = name.rpartition("::")
    cur = ""
    pat = re.compile(r"^\s*(?:virtual\s+|static\s+)?[\w:<>*&,\s]+?\b"
                     + re.escape(fn) + r"\s*\(")
    for line in bro.read_text(encoding="utf-8", errors="replace").splitlines():
        m = re.match(r"^\s*class\s+([\w:]+)", line)
        if m:
            cur = m.group(1)
        if cls and cur != cls:
            continue
        if pat.match(line):
            w = re.search(r"win\s+(0x[0-9a-fA-F]+)", line)
            if w:
                return int(w.group(1), 16)
            raise KeyError(f"{name} is inline on win (cannot be hooked)")
    raise KeyError(f"{name} is not in the bindings")


def resolve(tok: str) -> int:
    return int(tok, 16) if tok.lower().startswith("0x") else sym_addr(tok)


def sym_at(rva: int, bro: Path = BRO) -> str:
    u"""Address -> `Class::method` (reverse lookup, to name the target of a call)."""
    if not hasattr(sym_at, "_map"):
        m: dict[int, str] = {}
        cur = ""
        for line in bro.read_text(encoding="utf-8",
                                  errors="replace").splitlines():
            c = re.match(r"^\s*class\s+([\w:]+)", line)
            if c:
                cur = c.group(1)
            w = re.search(r"win\s+(0x[0-9a-fA-F]+)", line)
            f = re.search(r"\b(\w+)\s*\(", line)
            if w and f:
                m.setdefault(int(w.group(1), 16), f"{cur}::{f.group(1)}")
        sym_at._map = m                                        # type: ignore
    return sym_at._map.get(rva, "")                            # type: ignore


def main() -> int:
    global BRO
    ap = argparse.ArgumentParser()
    ap.add_argument("addr", nargs="?", default=None,
                    help="0x... or Class::method")
    ap.add_argument("--sym", default=None, help="same as addr, spelled out")
    ap.add_argument("--n", type=int, default=200, help="how many instructions")
    ap.add_argument("--xref", default=None,
                    help="print every site that calls this function")
    ap.add_argument("--float", dest="flt", default=None,
                    help="read the float / double at that address")
    ap.add_argument("--exe", default=str(EXE))
    ap.add_argument("--bro", default=str(BRO))
    a = ap.parse_args()
    BRO = Path(a.bro)
    pe = PE(Path(a.exe))

    if a.flt:
        rva = resolve(a.flt)
        off = pe.off(rva)
        if off is None:
            print("that address is not in the file (bss?)")
            return 1
        f, = struct.unpack_from("<f", pe.buf, off)
        d, = struct.unpack_from("<d", pe.buf, off)
        print(f"0x{rva:x}  float={f!r}  double={d!r}")
        return 0

    if a.xref:
        target = resolve(a.xref)
        base, code = pe.text()
        hits = []
        for i in range(len(code) - 5):
            if code[i] != 0xE8:                     # call rel32
                continue
            rel = struct.unpack_from("<i", code, i + 1)[0]
            if base + i + 5 + rel == target:
                hits.append(base + i)
        print(f"call 0x{target:x} ({a.xref}): {len(hits)} call sites:")
        for h in hits:
            print(f"  0x{h:x}")
        if not hits:
            print("  (none means a virtual call or an inline. A call through a "
                  "vtable slot is call qword ptr [rax+N], which this scan does "
                  "not see)")
        return 0

    tok = a.sym or a.addr
    if not tok:
        ap.error("an address or --sym is required")
    rva = resolve(tok)
    off = pe.off(rva)
    if off is None:
        print("that address is not in the file")
        return 1
    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
    data = pe.buf[off:off + a.n * 8]
    left = a.n
    for ins in md.disasm(data, rva):
        note = ""
        if ins.mnemonic == "call" and ins.op_str.startswith("0x"):
            nm = sym_at(int(ins.op_str, 16))
            if nm:
                note = f"   ; {nm}"
        print(f"0x{ins.address:x}  {ins.mnemonic:<10}{ins.op_str}{note}")
        left -= 1
        if left <= 0:
            break
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
