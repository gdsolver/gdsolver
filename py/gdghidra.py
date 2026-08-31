# -*- coding: utf-8 -*-
u"""Query the Ghidra project of GD's exe from the command line.

This is the whole-codebase companion to gddisasm.py: capstone stays the tool
for a quick linear read from a known head, Ghidra is for the questions that
span the binary — decompiled pseudocode of long functions, "who calls this",
"who references this address", and a function database that keeps its names
between sessions.

    python py/gdghidra.py --sync-broma                 # once per fresh project
    python py/gdghidra.py --decomp PlayerObject::collidedWithObject
    python py/gdghidra.py --decomp 0x2158f9            # any address inside it
    python py/gdghidra.py --xrefs PlayerObject::propellPlayer
    python py/gdghidra.py --xrefs 0x622bac             # data works too
    python py/gdghidra.py --funcs collision            # search names (regex)

The project lives in the private lab (GDSOLVER_LAB), holds the imported
worker exe, and is created once by a headless import + auto-analysis run
(see the lab notes for the exact command). Symbols come from the same
2.2081 Broma that gddisasm.py reads, so both tools agree on names.

Traps:
- Each invocation boots a JVM and opens the project (~10-20 s). Batch your
  questions, or keep using gddisasm.py for one-liners it can answer.
- MSVC folded identical functions (/OPT:ICF), so several Broma names can
  share one address. The first name becomes the function name, the rest are
  added as extra labels — search finds them, the decompiler shows the first.
- The headless analyzer and this tool take the project lock exclusively:
  one at a time.
"""
from __future__ import annotations

import argparse
import contextlib
import os
import re
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from gddisasm import BRO, resolve  # noqa: E402
from gdtas.paths import LAB  # noqa: E402

PROJECT_DIR = LAB / "ghidra"
PROJECT_NAME = "GD2.2081"

# pyghidra needs both of these before start(); the lab install is the default
# so a plain `python py/gdghidra.py` works on this machine, and either can be
# overridden from the environment for another layout.
os.environ.setdefault(
    "GHIDRA_INSTALL_DIR", str(LAB / "tools" / "ghidra_12.1.3_PUBLIC"))
os.environ.setdefault(
    "JAVA_HOME", str(LAB / "tools" / "jdk-21.0.12.1+1"))


def broma_symbols(bro: Path = BRO) -> dict[int, list[str]]:
    u"""{rva: ["Class::method", ...]} for every `win 0x...` in the Broma."""
    out: dict[int, list[str]] = {}
    cur = ""
    for line in bro.read_text(encoding="utf-8", errors="replace").splitlines():
        c = re.match(r"^\s*class\s+([\w:]+)", line)
        if c:
            cur = c.group(1)
        w = re.search(r"win\s+(0x[0-9a-fA-F]+)", line)
        f = re.search(r"\b(\w+)\s*\(", line)
        if w and f and cur:
            out.setdefault(int(w.group(1), 16), []).append(
                f"{cur}::{f.group(1)}")
    return out


@contextlib.contextmanager
def _program(save: bool = False):
    u"""Open the (already imported and analysed) program in the lab project."""
    import pyghidra
    pyghidra.start()
    project = pyghidra.open_project(str(PROJECT_DIR), PROJECT_NAME)
    try:
        files = list(project.getProjectData().getRootFolder().getFiles())
        gd = [f for f in files if f.getName().startswith("GeometryDash")]
        if not gd:
            raise SystemExit(f"no GeometryDash program in {PROJECT_DIR}; "
                             "run the headless import first (lab notes)")
        with pyghidra.program_context(project, gd[0].getPathname()) as program:
            yield program
            if save:
                from ghidra.util.task import TaskMonitor
                program.save("gdghidra", TaskMonitor.DUMMY)
    finally:
        project.close()


def _addr(program, rva: int):
    return program.getImageBase().add(rva)


def _namespace(program, path: str):
    u"""Get-or-create the (possibly nested) namespace for `A::B::C`."""
    from ghidra.program.model.symbol import SourceType
    st = program.getSymbolTable()
    ns = program.getGlobalNamespace()
    for part in path.split("::"):
        ns = st.getOrCreateNameSpace(ns, part, SourceType.IMPORTED)
    return ns


def cmd_sync_broma() -> int:
    syms = broma_symbols()
    named = created = labeled = missing = 0
    with _program(save=True) as program:
        # `ghidra.*` only exists once the JVM is up, i.e. after _program().
        from ghidra.program.flatapi import FlatProgramAPI
        from ghidra.program.model.symbol import SourceType
        flat = FlatProgramAPI(program)
        fm = program.getFunctionManager()
        tx = program.startTransaction("broma sync")
        try:
            for rva, names in sorted(syms.items()):
                addr = _addr(program, rva)
                fn = fm.getFunctionAt(addr)
                if fn is None:
                    fn = flat.createFunction(addr, "tmp")
                    if fn is None:
                        missing += 1
                        continue
                    created += 1
                cls, _, leaf = names[0].rpartition("::")
                ns = _namespace(program, cls)
                fn.getSymbol().setNameAndNamespace(
                    leaf, ns, SourceType.IMPORTED)
                named += 1
                # ICF-folded twins: keep the other names findable as labels.
                for extra in names[1:]:
                    ecls, _, eleaf = extra.rpartition("::")
                    program.getSymbolTable().createLabel(
                        addr, eleaf, _namespace(program, ecls),
                        SourceType.IMPORTED)
                    labeled += 1
        finally:
            program.endTransaction(tx, True)
    print(f"named {named} functions ({created} newly created), "
          f"{labeled} extra labels on folded twins, "
          f"{missing} addresses with no code")
    return 0


def cmd_decomp(tok: str, timeout: int) -> int:
    rva = resolve(tok)
    with _program() as program:
        from ghidra.app.decompiler import DecompInterface, DecompileOptions
        from ghidra.util.task import ConsoleTaskMonitor
        fn = program.getFunctionManager().getFunctionContaining(
            _addr(program, rva))
        if fn is None:
            print(f"no function contains 0x{rva:x} (run --sync-broma, or the "
                  "auto-analysis missed it)")
            return 1
        ifc = DecompInterface()
        ifc.setOptions(DecompileOptions())
        ifc.openProgram(program)
        res = ifc.decompileFunction(fn, timeout, ConsoleTaskMonitor())
        if not res.decompileCompleted():
            print(f"decompile failed: {res.getErrorMessage()}")
            return 1
        print(f"// {fn.getName(True)} @ base+0x{rva:x}")
        print(res.getDecompiledFunction().getC())
    return 0


def cmd_xrefs(tok: str) -> int:
    rva = resolve(tok)
    with _program() as program:
        addr = _addr(program, rva)
        fm = program.getFunctionManager()
        refs = list(program.getReferenceManager().getReferencesTo(addr))
        print(f"{len(refs)} references to base+0x{rva:x}:")
        for r in refs:
            src = r.getFromAddress()
            fn = fm.getFunctionContaining(src)
            where = fn.getName(True) if fn else "(no function)"
            off = src.subtract(program.getImageBase())
            print(f"  base+0x{off:x}  {r.getReferenceType()}  in {where}")
    return 0


def cmd_funcs(pattern: str) -> int:
    pat = re.compile(pattern, re.IGNORECASE)
    with _program() as program:
        base = program.getImageBase()
        n = 0
        for fn in program.getFunctionManager().getFunctions(True):
            name = fn.getName(True)
            if pat.search(name):
                print(f"  base+0x{fn.getEntryPoint().subtract(base):x}  {name}")
                n += 1
        print(f"{n} functions match /{pattern}/i")
    return 0


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--sync-broma", action="store_true",
                    help="name every `win 0x...` function from the Broma")
    ap.add_argument("--decomp", default=None,
                    help="0x... or Class::method -> decompiled pseudocode")
    ap.add_argument("--xrefs", default=None,
                    help="0x... or Class::method -> every reference to it")
    ap.add_argument("--funcs", default=None,
                    help="regex -> matching function names")
    ap.add_argument("--timeout", type=int, default=120,
                    help="decompiler seconds per function")
    a = ap.parse_args()
    if a.sync_broma:
        return cmd_sync_broma()
    if a.decomp:
        return cmd_decomp(a.decomp, a.timeout)
    if a.xrefs:
        return cmd_xrefs(a.xrefs)
    if a.funcs:
        return cmd_funcs(a.funcs)
    ap.print_help()
    return 2


if __name__ == "__main__":
    raise SystemExit(main())
