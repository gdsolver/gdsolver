#!/usr/bin/env python
"""Verify that an edit touched only comments.

    python tools/check_code_only_diff.py <path> [<path> ...]

For each path, the working copy is compared against `git show HEAD:<path>` with
comments removed and trailing whitespace normalised:

* C/C++ (.c .cc .cpp .h .hpp .inc): `//` and `/* */`. String and character
  literals are kept verbatim, so a change inside a printf format counts as a
  code change.
* Python (.py): `#` comments and *docstrings* (the leading string statement of
  a module, class or function). Every other string literal is kept verbatim —
  a printed message is part of the program's behaviour, not documentation.

Exit status 0 = code identical for every path.
"""
from __future__ import annotations

import ast
import io
import subprocess
import sys
import tokenize
from pathlib import Path


def strip_comments(src: str) -> list[str]:
    out: list[str] = []
    src = src.lstrip("﻿")          # a BOM is not a code change
    i, n = 0, len(src)
    buf: list[str] = []
    while i < n:
        c = src[i]
        if c == '"' or c == "'":
            q = c
            buf.append(c)
            i += 1
            while i < n and src[i] != q:
                if src[i] == "\\" and i + 1 < n:
                    buf.append(src[i]); buf.append(src[i + 1]); i += 2
                    continue
                if src[i] == "\n":          # unterminated literal: bail out
                    break
                buf.append(src[i]); i += 1
            if i < n and src[i] == q:
                buf.append(q); i += 1
            continue
        if c == "/" and i + 1 < n and src[i + 1] == "/":
            while i < n and src[i] != "\n":
                i += 1
            continue
        if c == "/" and i + 1 < n and src[i + 1] == "*":
            i += 2
            while i + 1 < n and not (src[i] == "*" and src[i + 1] == "/"):
                if src[i] == "\n":
                    buf.append("\n")        # keep line structure
                i += 1
            i += 2
            continue
        buf.append(c)
        i += 1
    for line in "".join(buf).split("\n"):
        s = line.rstrip()
        if s.strip():
            out.append(s)
    return out


def strip_python(src: str) -> list[str]:
    """Blank out `#` comments and docstrings, keep every other token."""
    lines = src.split("\n")
    drop: set[int] = set()          # 1-based line numbers to blank entirely
    try:
        tree = ast.parse(src)
    except SyntaxError:
        tree = None
    if tree is not None:
        for node in ast.walk(tree):
            if not isinstance(node, (ast.Module, ast.ClassDef, ast.FunctionDef,
                                     ast.AsyncFunctionDef)):
                continue
            body = getattr(node, "body", None)
            if not body:
                continue
            first = body[0]
            if (isinstance(first, ast.Expr) and isinstance(first.value, ast.Constant)
                    and isinstance(first.value.value, str)):
                for ln in range(first.lineno, (first.end_lineno or first.lineno) + 1):
                    drop.add(ln)
    try:
        toks = list(tokenize.generate_tokens(io.StringIO(src).readline))
    except (tokenize.TokenError, IndentationError):
        toks = []
    cut: dict[int, int] = {}        # line -> column where a comment starts
    for tok in toks:
        if tok.type == tokenize.COMMENT:
            ln, col = tok.start
            cut[ln] = min(col, cut.get(ln, col))
    out = []
    for i, line in enumerate(lines, 1):
        if i in drop:
            continue
        if i in cut:
            line = line[:cut[i]]
        s = line.rstrip()
        if s.strip():
            out.append(s)
    return out


def head_version(path: str) -> str:
    rel = Path(path).resolve().relative_to(Path(subprocess.check_output(
        ["git", "rev-parse", "--show-toplevel"], text=True).strip()).resolve())
    return subprocess.check_output(["git", "show", f"HEAD:{rel.as_posix()}"],
                                   text=True, encoding="utf-8", errors="replace")


def main(argv: list[str]) -> int:
    bad = 0
    for p in argv:
        strip = strip_python if Path(p).suffix == ".py" else strip_comments
        before = strip(head_version(p))
        after = strip(Path(p).read_text(encoding="utf-8", errors="replace"))
        if before == after:
            print(f"CODE IDENTICAL  {p}  ({len(after)} code lines)")
            continue
        bad += 1
        print(f"CODE CHANGED    {p}")
        for k, (a, b) in enumerate(zip(before, after)):
            if a != b:
                print(f"  first difference at code line {k + 1}:")
                print(f"    HEAD: {a}")
                print(f"    now : {b}")
                break
        else:
            print(f"  line count differs: HEAD {len(before)} vs now {len(after)}")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
