"""Which `g_*` global carries state out of one solve and into the next?

`dp::cliMain` was written as a process: parse a command line, load a level,
search, print, exit. Under that assumption a namespace-scope global IS
per-solve state and nothing ever had to clear one. The mod broke it -- it calls
cliMain in-process dozens of times per level (src/mod/dp_bridge.cpp), and the
cold suite's `--one-session` runs all 22 levels in one game -- so every such
global now reaches the NEXT call. `dp/src/dp/reset.hpp` is the answer to that,
and `p1::resetSessionState` (src/mod/plan_io.hpp) is the mod's own copy of the
same contract.

Both are hand-maintained lists, which is the whole problem: a global landed
without a line there leaks silently, and a launch-per-level regression can
never see it. CLAUDE.md records one that sat under a green 22/22 for months
(one level's moving-geometry recording adopted by the next), and on 2026-09-04
three more landed the same way (g_anchorState, g_seedPartialOk, g_seedPartial,
fixed in 9ceaabe). This script is the guard.

What it does:

  1. enumerates every namespace-scope `g_*` variable under dp/src/dp and src,
     namespace-qualified, with type and initialiser;
  2. computes what each reset entry point actually clears, by TRANSITIVE
     CLOSURE over the functions it calls (see RESET COVERAGE below -- a plain
     grep of reset.hpp is wrong for the mod, whose reset delegates to
     grouptrace::reset, secsolve::reset, itermap::clear and friends);
  3. checks the remainder against the allowlist below, which records a human's
     classification of each one, and exits non-zero on anything not accounted
     for -- so adding a global forces a decision rather than passing silently.

RESET COVERAGE -- method and its false-negative risks
-----------------------------------------------------
Name matching alone is not enough, and each of these has bitten:

  * comments.   reset.hpp names g_levelCsv, g_progress and g_rotCompute in
                prose only. Stripped before matching, or they read as covered.
  * delegation. resetSessionState clears most of the mod's state through
                calls. Followed transitively, entry points below.
  * loops.      `for (auto& f : g_frameLv) f.reset();` and
                `for (int b = 0; b < 32; ++b) g_touchFireT[b] = -1;` never put
                the name next to a plain `=`. Matched as range-for subject and
                as subscripted assignment.
  * struct-wide assignment. `g_cfg = Config{}` (hooks_playlayer.cpp) resets a
                whole struct's worth of settings in one statement. The members
                are not globals so they are out of scope here, but the same
                shape would hide a global inside an aggregate -- and would be
                missed. No such case exists today; MEMBERWISE below is the
                place to record one if it appears.
  * reads.      A name appearing in a reset body in a NON-mutating position is
                not coverage. Reported separately (`touched but not mutated`)
                rather than counted.

Residual risk this cannot cover: a global mutated through a reference or
pointer alias, or cleared by a function reached through a function pointer.
`inline bool& g_active = g_secSearching;` (src/solver/secsolve.hpp:313) is the
one alias in the tree; it is in the mod scope, which is on a ratchet rather
than classified, so nothing here rests on it.

VERIFYING THE INSTRUMENT
------------------------
An audit that reports nothing because it matched nothing is worse than no
audit, and this one has produced that answer twice while being written (once
by excluding every file in the tree, once by calling every global constant).
So it is checked against a commit whose answer is known: at 8f1ae6b the three
globals 9ceaabe went on to reset were live defects, and

    python tools/reset_coverage.py --root <checkout of 8f1ae6b>

must report exactly g_anchorState, g_seedPartial and g_seedPartialOk more than
the same run against 9ceaabe -- no fewer, and nothing else. It does. The
built-in self-check covers the other half: every `g_` token in the tree must
resolve to a declaration the scanner parsed, so a declaration form it cannot
read is reported instead of silently dropped (that check found six globals
missing from the first draft's table).

WHAT THE CLOSURE IS NOT
-----------------------
The audit that produced this file also asked, by hand and separately, "who
calls each of the mod's reset functions". That question was asked with a grep
whose namespace alternation was typed from memory --
`(dpsolve|secsolve|grouptrace|psnap|itermap|probe|clearance)::(reset|clear)`
-- and `anchors` was not in it, so `anchors::reset()` (src/mod/repair.hpp:117,
declared under `namespace anchors {` at :95) was reported as having no caller.
It has one: repair.hpp:2202, in the level-start block immediately above the
`dpsolve: start level=` line, with `anchors::onAttemptStart()` at
hooks_playlayer.cpp:184 as its per-attempt counterpart.

That miss cannot reach the verdicts here, and the reason is structural rather
than lucky. It was a BACKWARD query (who calls X) driven by a hand-enumerated
list of names. reset_set() runs FORWARD (what does X call, transitively) over
a generic `(?:(\w+)\s*::\s*)?(\w+)\s*\(`, so there is no name list in this
file to leave something out of -- it resolves probe::reset, secsolve::reset,
grouptrace::reset, padtrace::reset, orbtrace::reset and speedgate::reset
without ever being told they exist. And a missed CALLER cannot change what a
function DOES: every claim here has the form "the reset body does not mutate
this name", which depends only on that body's contents.

For dp/ the point is sharper still. resetInvocationState calls no free
function at all -- the closure enters exactly one body, its own, and follows
zero call edges -- so the dp verdicts, the only ones this file classifies,
involve no call resolution whatsoever. (Were a call edge ever missed, the
error direction is worth knowing: fewer resolved resets means MORE globals
reported unreset -- false findings rather than silence.)

    python tools/reset_coverage.py            # audit, exit 1 on anything new
    python tools/reset_coverage.py --list     # every global and its verdict
    python tools/reset_coverage.py --writers RE  # the assignments behind a bucket
    python tools/reset_coverage.py --root DIR # audit another checkout
"""
import argparse
import re
import sys
from pathlib import Path

# ---------------------------------------------------------------------------
# Scopes: a source root, and the reset entry point that governs it.
# ---------------------------------------------------------------------------
SCOPES = [
    # dp/ globals are per-solve state; cliMain calls resetInvocationState
    # first. Classified one by one in ALLOWLIST_DATA -- this is the scope the
    # audit of 2026-09-04 covered.
    ("dp", "dp/src/dp", ["resetInvocationState"], "allowlist"),
    # src/ globals are per-session state; PlayLayer::onQuit calls
    # resetSessionState (and hooks_playlayer.cpp then does `g_cfg = Config{}`).
    #
    # NOT individually classified, and deliberately so. The mod's state has
    # several lifetimes -- per session, per attempt, per solve, per process --
    # with reset points to match (anchors::reset, dpsolve's own, itermap::clear
    # ...), so "not cleared by resetSessionState" is not by itself a defect the
    # way it is in dp/. Classifying 300-odd of them by guesswork would produce
    # a table that reads as evidence and is not. So this scope runs as a
    # RATCHET instead: the names below were the uncleared set on 2026-09-04,
    # and the audit fails on any name that is not one of them. That still
    # catches the thing this script is for -- a NEW global landing without a
    # reset -- without asserting anything about the 300 that were already
    # there. Retiring a name from the ratchet into a real classification is
    # the way this shrinks.
    ("mod", "src", ["resetSessionState"], "ratchet"),
]
SUFFIXES = {".hpp", ".cpp", ".h", ".inc", ".cc"}

# Globals reset by a whole-struct assignment rather than by name. Empty today;
# if a global is ever folded into an aggregate that is reset as a unit, name it
# here with the statement that does it, or the audit will call it a leak.
MEMBERWISE: dict[str, str] = {}

# `g_`-prefixed names that are NOT namespace-scope globals -- locals, struct
# members, or names that live outside the two scanned roots. Anything else the
# self-check cannot resolve is a scanner defect, not an entry for this set.
NOT_GLOBALS: set[str] = set()


# ---------------------------------------------------------------------------
# Lexing helpers
# ---------------------------------------------------------------------------
def strip_comments(text: str) -> str:
    """Blank out comments and string/char literals, preserving newlines so that
    line numbers computed from offsets stay correct."""
    out = []
    i, n = 0, len(text)
    while i < n:
        c = text[i]
        if c == "/" and i + 1 < n and text[i + 1] == "/":
            j = text.find("\n", i)
            j = n if j < 0 else j
            out.append(" " * (j - i))
            i = j
        elif c == "/" and i + 1 < n and text[i + 1] == "*":
            j = text.find("*/", i + 2)
            j = n if j < 0 else j + 2
            out.append("".join(ch if ch == "\n" else " " for ch in text[i:j]))
            i = j
        elif c in "\"'":
            j = i + 1
            while j < n and text[j] != c:
                j += 2 if text[j] == "\\" else 1
            j = min(j + 1, n)
            out.append("".join(ch if ch == "\n" else " " for ch in text[i:j]))
            i = j
        else:
            out.append(c)
            i += 1
    return "".join(out)


def strip_directives(text: str) -> str:
    """Blank preprocessor lines (with their backslash continuations).

    Only for DECLARATION scanning -- never for use scanning, because a macro
    body is a real writer (`#define CLAMP0O(...) do { g_clampWhy = ...` in
    step.hpp assigns four globals and must count).

    Why declarations need it: a macro body ends `} while (0)` with NO
    semicolon, so the statement the scanner assembled for the NEXT line began
    with `while` and the keyword filter threw the declaration away. That is
    how g_deadCx/g_deadCy (step.hpp:83) stayed outside the audit."""
    out, lines = [], text.split("\n")
    cont = False
    for ln in lines:
        if cont or ln.lstrip().startswith("#"):
            cont = ln.rstrip().endswith("\\")
            out.append("")
        else:
            out.append(ln)
    return "\n".join(out)


def split_top(s: str, seps: str) -> list[str]:
    """Split on `seps` that are not nested in (), [], {} or <>."""
    parts, depth, ang, cur = [], 0, 0, []
    for ch in s:
        if ch in "([{":
            depth += 1
        elif ch in ")]}":
            depth -= 1
        elif ch == "<":
            ang += 1
        elif ch == ">":
            ang = max(0, ang - 1)
        if ch in seps and depth == 0 and ang == 0:
            parts.append("".join(cur))
            cur = []
        else:
            cur.append(ch)
    parts.append("".join(cur))
    return parts


NAME = r"g_[A-Za-z]\w*"
DECL_HEAD = re.compile(
    r"^\s*(?:inline\s+|static\s+|thread_local\s+|constexpr\s+|const\s+|extern\s+)*"
    r"[A-Za-z_]")
SKIP_KW = re.compile(
    r"^\s*(?:using|namespace|struct|class|enum|template|typedef|friend|return|"
    r"if|for|while|else|case|do|switch|#)\b")


def scan_globals(root: Path, base: Path) -> dict:
    """name -> dict(file, line, ns, type, init, qual). Namespace-scope only."""
    found = {}
    for path in sorted(root.rglob("*")):
        if not path.is_file() or path.suffix not in SUFFIXES:
            continue
        raw = path.read_text(encoding="utf-8", errors="replace")
        text = strip_directives(strip_comments(raw))
        rel = str(path.relative_to(base)).replace("\\", "/")

        # Walk braces, remembering which ones opened a namespace. A statement is
        # at namespace scope only while no ordinary brace (function, struct,
        # initialiser) is open -- that is what keeps locals out of the table.
        stack = []          # list of ("ns"|"other"|"init", name)
        stmt_start = 0
        i, n = 0, len(text)
        while i < n:
            ch = text[i]
            if ch == "{":
                head = text[stmt_start:i]
                m = re.search(r"\bnamespace\s+([\w:]+)?\s*$", head)
                if stack and stack[-1][0] == "init":
                    # Inside an aggregate initialiser every nested brace is
                    # part of it. Without this, the "bare block" rule (a `{`
                    # right after another `{`) claimed the inner braces of
                    # `inline Bucketed g_solidIdx{{}, 0, false};` and of
                    # hookdepth's `g_slots[] = { {...}, {...} }`, restarting
                    # the statement mid-initialiser and losing all three.
                    stack.append(("init", None))
                elif m:
                    stack.append(("ns", m.group(1) or ""))
                    stmt_start = i + 1
                elif _is_block(head):
                    stack.append(("other", None))
                    stmt_start = i + 1
                else:
                    # An AGGREGATE INITIALISER, not a block: `inline int
                    # g_refKidFate[2] = {-1, -1};`. Treating it as a block
                    # restarted the statement after the `}`, so the statement
                    # tested at the `;` was empty and the declaration vanished
                    # -- four globals (g_refKidFate, g_refKidWhy, g_refKidKey,
                    # g_refKidState, refwatch.hpp:83-86) were missing from the
                    # table with nothing to show for it. Leave stmt_start alone.
                    stack.append(("init", None))
            elif ch == "}":
                top = stack.pop() if stack else ("other", None)
                if top[0] != "init":
                    stmt_start = i + 1
            elif ch == ";":
                if all(k == "ns" for k, _ in stack):
                    _decl(text, stmt_start, i, stack, rel, found)
                stmt_start = i + 1
            i += 1
    return found


BLOCK_HEAD = re.compile(
    r"(?:\bnamespace\b[\w:\s]*"
    r"|\)\s*(?:const\s*)?(?:noexcept\s*)?(?:->[^{;]*)?"
    r"|\b(?:struct|class|union)\s+\w+(?:\s*:[^{;]*)?"
    r"|\benum(?:\s+class)?\s+\w+(?:\s*:[^{;]*)?"
    r"|\b(?:else|do|try)"
    r"|extern\s+\"C\""
    r"|^\s*|[{};])\s*$")


def _is_block(head: str) -> bool:
    """Does this `{` open a block, or an initialiser? Only blocks restart the
    statement."""
    return bool(BLOCK_HEAD.search(head))


def _decl(text, a, b, stack, rel, found):
    stmt = text[a:b]
    if "g_" not in stmt or SKIP_KW.search(stmt) or not DECL_HEAD.match(stmt):
        return
    # A declaration's declarators are the comma-separated segments after the
    # type; in each, the first g_ name BEFORE any `=` is the thing declared
    # (`inline bool& g_active = g_secSearching;` declares g_active, not the
    # initialiser it aliases; `inline size_t g_rotSeen = 0, g_rotRouted = 0;`
    # declares both -- that multi-declarator form holds 26 of this tree's
    # globals and a one-name-per-line regex misses every one).
    ns = "::".join(x for k, x in stack if k == "ns" and x)
    ty0 = None
    off = 0
    for seg in split_top(stmt, ","):
        lhs = split_top(seg, "=")[0]
        # ...optionally followed by a braced initialiser with no `=` at all:
        # `inline State g_refKidState[2]{};` and the three `std::array` queues
        # in frames.hpp are declared that way.
        # ...and the brace match is greedy, because these initialisers nest:
        # `inline Bucketed g_hazardIdx{{}, 2, false};` (clearance.hpp).
        m = re.search(r"\b(" + NAME + r")\s*(?:\[[^\]]*\])?\s*(?:\{.*\})?\s*$",
                      lhs, re.S)
        if m:
            name = m.group(1)
            ty = " ".join(lhs[:m.start(1)].split())
            ty = re.sub(r"^(?:inline|static|constexpr|extern)\s+", "",
                        ty).strip()
            # Only the FIRST declarator carries the type; later ones in
            # `inline size_t g_rotSeen = 0, g_rotRouted = 0;` inherit it.
            if ty0 is None:
                ty0 = ty
            elif not ty:
                ty = ty0
            if name not in found:         # first declaration wins
                init = seg[len(lhs) + 1:].strip() if "=" in seg else ""
                # line number from the NAME's absolute offset, not the
                # statement's: `a` sits just past the previous `;`, i.e. on the
                # previous line, which reported every global one line early.
                pos = a + off + m.start(1)
                found[name] = dict(
                    file=rel, line=text.count("\n", 0, pos) + 1, ns=ns,
                    type=ty or ty0 or "?", init=" ".join(init.split())[:60],
                    qual=f"{ns}::{name}" if ns else name)
        off += len(seg) + 1               # +1 for the comma split_top removed


# ---------------------------------------------------------------------------
# Reset coverage: transitive closure from the entry points
# ---------------------------------------------------------------------------
FUNC = re.compile(r"\binline\s+(?:void|bool|int)\s+(\w+)\s*\([^;{]*\)\s*\{")

MUTATE = [
    # plain / subscripted assignment, but never `==`; and the compound forms.
    # `++g_rotSeen` and `++g_fireBNoTick` were being reported as reads, which
    # made two accumulating counters look like constants.
    lambda n: re.compile(r"\b" + n + r"\s*(?:\[[^\]]*\])?\s*"
                         r"(?:[-+*/|&^%]|<<|>>)?=(?!=)"),
    lambda n: re.compile(r"(?:\+\+|--)\s*\b" + n + r"\b|\b" + n +
                         r"\b\s*(?:\+\+|--)"),
    # the container idioms reset.hpp uses
    lambda n: re.compile(r"\b" + n +
                         r"\s*\.\s*(?:clear|reset|fill|assign|close|erase|"
                         r"resize|swap|pop_back)\s*\("),
    # `for (auto& f : g_frameLv) f.reset();`
    lambda n: re.compile(r":\s*" + n + r"\s*\)"),
]


def collect_bodies(root: Path) -> dict:
    """(namespace, funcname) -> body text, comments stripped."""
    bodies = {}
    for path in sorted(root.rglob("*")):
        if not path.is_file() or path.suffix not in SUFFIXES:
            continue
        text = strip_comments(path.read_text(encoding="utf-8",
                                             errors="replace"))
        for m in FUNC.finditer(text):
            # brace-match the body
            i, depth = m.end() - 1, 0
            while i < len(text):
                if text[i] == "{":
                    depth += 1
                elif text[i] == "}":
                    depth -= 1
                    if depth == 0:
                        break
                i += 1
            body = text[m.end():i]
            ns = ""
            for nm in re.finditer(r"\bnamespace\s+([\w:]+)\s*\{", text[:m.start()]):
                ns = nm.group(1)          # innermost preceding; good enough here
            bodies.setdefault((ns, m.group(1)), body)
            bodies.setdefault(("", m.group(1)), body)
    return bodies


def reset_set(bodies: dict, entries: list, names: set):
    """Names mutated by the entry points or anything they call.

    Returns (mutated, touched_only) -- the second is names that appear in a
    reset body but never in a mutating position, which is NOT coverage and is
    worth printing rather than silently counting either way."""
    mutated, touched, seen = set(), set(), set()
    queue = [("", e) for e in entries]
    while queue:
        key = queue.pop()
        if key in seen:
            continue
        seen.add(key)
        body = bodies.get(key)
        if body is None:
            continue
        for n in names:
            # Word-boundary, not `in`: a substring test made g_at "touched" by
            # every `g_attempt`, g_fix by `g_fixups`, g_total by
            # `g_totalAttempts` -- ten phantom entries in the report.
            if not re.search(r"\b" + n + r"\b", body):
                continue
            touched.add(n)
            if any(pat(n).search(body) for pat in MUTATE):
                mutated.add(n)
        for cm in re.finditer(r"(?:(\w+)\s*::\s*)?(\w+)\s*\(", body):
            queue.append((cm.group(1) or "", cm.group(2)))
    return mutated, touched - mutated


def find_uses(base: Path, names: set) -> dict:
    """name -> [(file:line, statement, mutating?)] for every occurrence outside
    its own declaration, across the WHOLE tree.

    Two lessons are baked in here.

    Scope: this used to search only the scope's own root, which reported
    dp::g_levelCsv as never assigned -- its writer is src/mod/dp_bridge.cpp,
    in the other root. A global's writers are wherever they are.

    Reads count. The first cut called a global constant when no MUTATE pattern
    matched, and that is not a proof: dp::g_progress is only ever mutated
    through `.begin()` / `.end()` / `.layer()`, none of which any assignment
    pattern can see, and it came back "provably never assigned". So the only
    thing claimed automatically now is the honest one -- the name occurs
    NOWHERE outside its declaration, so nothing writes it and nothing reads it.
    Everything else is a judgement and needs a line in the allowlist."""
    out = {n: [] for n in names}
    pats = {n: (re.compile(r"\b" + n + r"\b"),
                [p(n) for p in MUTATE]) for n in names}
    for path in sorted(base.rglob("*")):
        if not path.is_file() or path.suffix not in SUFFIXES:
            continue
        rel_parts = path.relative_to(base).parts
        # RELATIVE parts, not path.parts: the audit is normally run from a git
        # worktree under `.claude/worktrees/...`, so testing the absolute path
        # skipped every file in the tree and the whole report came back
        # "used nowhere at all" -- the silent-empty-match failure exactly.
        if rel_parts and rel_parts[0] in (".claude", ".git", "build",
                                          "build-dp", "data"):
            continue
        text = strip_comments(path.read_text(encoding="utf-8",
                                             errors="replace"))
        lines = text.splitlines()
        rel = str(path.relative_to(base)).replace("\\", "/")
        for n, (any_pat, mut_pats) in pats.items():
            if n not in text:
                continue
            for m in any_pat.finditer(text):
                ln = text.count("\n", 0, m.start()) + 1
                line = lines[ln - 1].strip() if ln <= len(lines) else ""
                # its own declaration is not a use
                if re.match(r"^\s*(?:inline|static|thread_local|constexpr|"
                            r"extern)\b", line) and re.search(
                                r"\b" + n + r"\b\s*(?:\[[^\]]*\])?\s*(?:=|,|;)",
                                line):
                    continue
                mut = any(p.search(line) for p in mut_pats)
                out[n].append((f"{rel}:{ln}", line[:110], mut))
    return out


# ---------------------------------------------------------------------------
# ALLOWLIST -- a human's verdict on every global the reset does not clear.
#
# Buckets:
#   B  configuration set from argv (or a config file) on EVERY invocation,
#      unconditionally, so a stale value cannot survive. The reason string
#      names the assignment site that proves it. A value assigned only when a
#      flag is PRESENT is not B -- it is A.
#   C  genuinely constant at runtime: never written after initialisation, or
#      written only by build/debug scaffolding that no run reaches.
#   S  session/process-lifetime state by design: it is deliberately not
#      per-level, and the reset would be the bug. Reason says whose lifetime.
#
# Anything not listed here is reported as UNACCOUNTED and fails the run. That
# is the point: a new global must be classified by a person.
# ---------------------------------------------------------------------------
ALLOW: dict[str, tuple] = {}


def load_allowlist():
    """Populated from ALLOWLIST_DATA below; kept as data so the table reads as
    a table."""
    for line in ALLOWLIST_DATA.strip().splitlines():
        line = line.strip()
        if not line or line.startswith("#"):
            continue
        bucket, name, reason = line.split(None, 2)
        ALLOW[name] = (bucket, reason)


ALLOWLIST_DATA = """
# bucket  name  reason
#
# ---- dp/ : governed by dp::resetInvocationState (dp/src/dp/reset.hpp) -------
#
# (B) the three deliberate exceptions. Two are documented in reset.hpp itself;
#     the third is cleared by cliMain one line above the reset call.
B g_levelCsv        set unconditionally at src/mod/dp_bridge.cpp:52 before every in-process cliMain and cleared at :61 -- the caller's data, not the last solve's (reset.hpp documents it)
B g_progress        a live readout the mod polls from another thread; cliMain re-opens it with g_progress.begin() at cli.hpp:2398 and a ProgressGuard ends it (reset.hpp documents it)
B g_outcome         cleared by cliMain itself at cli.hpp:41, deliberately ABOVE resetInvocationState so an early return leaves "FAILED, nothing measured" rather than the last verdict
#
# (B) per-tick step scratch, thread_local. cli.hpp:2218-2223 zeroes all six
#     immediately before every stepBoth call, so nothing can read an older
#     tick's value, let alone an older invocation's.
B g_nearOrb         zeroed every tick at cli.hpp:2218 before stepBoth
B g_dashVySet       zeroed every tick at cli.hpp:2219 before stepBoth
B g_clampWhy        zeroed every tick at cli.hpp:2220 before stepBoth
B g_clampUid        zeroed every tick at cli.hpp:2221 before stepBoth
B g_deadWhy         zeroed every tick at cli.hpp:2222 before stepBoth
B g_deadObj         zeroed every tick at cli.hpp:2223 before stepBoth
#
# (B) the payloads those six gate. Each is written only beside its own marker
#     -- g_dashVy with g_dashVySet (step.hpp:8836-8837), g_clampC* with
#     g_clampUid (the CLAMP0O macro, step.hpp:77-79), g_deadC* with g_deadWhy
#     and g_deadObj (the DIE macro, step.hpp:84-86) -- and every reader tests
#     the marker first, so a stale payload is unreachable.
B g_dashVy          read only under `g_dashVySet ?` at cli.hpp:2307; written beside it at step.hpp:8836
B g_clampCx         written only by the CLAMP0O macro, which sets the per-tick-cleared g_clampUid in the same statement (step.hpp:78-79)
B g_clampCy         written only by the CLAMP0O macro, which sets the per-tick-cleared g_clampUid in the same statement (step.hpp:78-79)
B g_deadCx          written only by the DIE macro, which sets the per-tick-cleared g_deadWhy/g_deadObj in the same statement (step.hpp:84-86)
B g_deadCy          written only by the DIE macro, which sets the per-tick-cleared g_deadWhy/g_deadObj in the same statement (step.hpp:84-86)
#
# (C) unreachable, so a stale value cannot differ from the declared default.
C g_rotCompute      declared false and the ONLY writer is `--no-rotcompute` setting it false again (cli.hpp:149); nothing can make it true, so it is constant by reachability (the known dead-flag shape)
C g_rotSeen         incremented only inside `if (g_rotCompute && ...)` at level_loader.hpp:354-357, which never runs -- permanently 0. Becomes (A) the day g_rotCompute is revived
C g_rotRouted       incremented only inside `if (g_rotCompute && ...)` at level_loader.hpp:354-362, which never runs -- permanently 0. Becomes (A) the day g_rotCompute is revived
#
# (A) PER-INVOCATION STATE THAT NOTHING CLEARS. These are the defect. Each one
#     belongs in resetInvocationState; listing it here keeps the run red until
#     it is either reset or re-argued. Ordered worst first.
#
A g_touchBoxU       REAL PER-LEVEL DATA, and the only member of its family nothing clears (g_touch, g_touchMoveTicks, g_touchFrame and g_touchFireT are all in reset.hpp). Touch-box travel coordinates filled from THIS level at triggers.hpp:457-458, read by ownTouch's 400px proximity gate at dynamics.hpp:838/893/1034. The fill is `b < 32 && b < out.size()`, so a level with fewer boxes than the previous one keeps the previous level's coordinates at indices >= out.size(). NOT SHOWN TO FIRE TODAY: all three readers index by a bit of State::trig, and this level's loader only ever assigns bits below out.size(), so the stale tail is currently unreachable. It becomes live the moment a trig bit outgrows its level's box count -- which the anchor path already shifts by re-windowing `out` (triggers.hpp:437-448) -- or a reader drops the bit gate
A g_rotSplit        declared TRUE and `--no-rotsplit` (cli.hpp:152) sets it false with nothing to set it back, so one invocation passing that flag disables rotation-orbit splitting for every later solve in the process
A g_refWatch        `--refwatch` sets it true (cli.hpp:114) and the search itself sets it false (cli.hpp:2421); gates the kid diagnostics at cli.hpp:2767/2785/3110/3270
A g_refEps          `--refeps` overwrites it (cli.hpp:119) only when the flag is present; the matching tolerance at refwatch.hpp:161-162 otherwise keeps the previous call's value
A g_refRows         the reference trace, loaded with `g_refRows[...] = r` (refwatch.hpp:125) and never cleared, so a second load MERGES two levels' reference rows into one map
A g_refParent       per-run watch state written during the search (cli.hpp:2412/3171/3278)
A g_refLostAt       per-run watch state written during the search (cli.hpp:3217/3276); `g_refLostAt < 0` is the "still tracking" test at cli.hpp:2406/3110/3270
A g_refDriftY       per-run watch state written during the search (cli.hpp:3197)
A g_refDriftVy      per-run watch state written during the search (cli.hpp:3199)
A g_refKidState     per-tick watch state written at cli.hpp:2793 -- and unlike its three siblings it is NOT re-initialised in the per-tick block at cli.hpp:2428-2430
A g_refKidFate      per-tick watch state (cli.hpp:2428/2790)
A g_refKidWhy       per-tick watch state (cli.hpp:2429/2791)
A g_refKidKey       per-tick watch state (cli.hpp:2430/2792)
A g_rotCheck        `--rotcheck` sets it true (cli.hpp:146) only when present; read at level_loader.hpp:1477
A g_shiftStat       `--shiftstat` sets it true (cli.hpp:104) only when present; read at dynamics.hpp:789/1328
A g_fireBCheck      `--firebcheck` sets it true (cli.hpp:86) only when present; gates the counters below at cli.hpp:2744
A g_fireBNoTick     `++` counter (cli.hpp:2748) that nothing zeroes, so the total printed at cli.hpp:3504 is the process's running total rather than this solve's
A g_fireBNoBit      `++` counter (cli.hpp:2749) that nothing zeroes -- same running-total defect
A g_fireBTooEarly   `++` counter (cli.hpp:2760) that nothing zeroes -- same running-total defect
"""

# Buckets that mean "accounted for and safe". Everything else in the allowlist
# is a finding that stays on the report and keeps the exit status non-zero.
SAFE_BUCKETS = {"B", "C", "S"}

# ---------------------------------------------------------------------------
# RATCHET -- src/ globals not cleared by resetSessionState as of 2026-09-04.
#
# This is a BASELINE, not a verdict: no claim is made that any name here is
# safe. It exists so that a global landing tomorrow fails the audit even
# though the 300 already here have not been classified. Shrink it by moving
# names into a real classification, never by adding to it without a reason.
# ---------------------------------------------------------------------------
RATCHET = """
  g_accel g_advance g_all g_anchor
  g_anchorT g_anchorX g_argsLogged g_at
  g_attemptStart g_barTicks g_best g_bestDeath
  g_bgBlocked g_cap g_capTier g_cfg
  g_ckptDash g_ckptNextInput g_ckptNextToggle g_ckptOobLatch
  g_ckptVy g_ckptVyRel g_clearMargin g_coinPickupTick
  g_coins g_coldRestarted g_colLog g_colProbe
  g_cpEndTick g_cpInjected g_cpN g_cpT
  g_cpVy g_cpY0 g_cpYStep g_csv
  g_cur g_curBackoff g_curSeenShowing g_dataDirResolved
  g_dataDirStorage g_dead g_deadlineSec g_deathBooked
  g_deathFx g_deathFxSkipped g_deathRuns g_deathsOther
  g_deathsP1 g_deathsP2 g_deathTail g_deepActive
  g_deepDoneAt g_deepest g_deepStartTick g_depth
  g_died g_dragFrac g_dt g_dualSeen
  g_endPurge g_endzoneBurnOn g_endzoneStart g_episodes
  g_escalations g_finished g_fix g_fixupCount
  g_fixupNoop g_fixupNoopPath g_fixupPath g_flagLines
  g_flownPlan g_followForced g_followSolved g_forceAnchorT
  g_forceCleanStart g_forcePortalBand g_fxSweep g_fxSwept
  g_gate g_gateHdrDone g_gateObjs g_gateStride
  g_gateT0 g_gateT1 g_gateTrace g_gateX0
  g_gateX1 g_goalX g_grace g_groupsBootPath
  g_groupsDeepPath g_groupsDepth g_groupsPath g_haveNewPlan
  g_hazardIdx g_hazards g_heapCheckEvery g_heldChannel
  g_heldFade g_heldLoop g_heldPath g_hidden
  g_horizon g_horizonFull g_horizonNow g_hot
  g_hotGen g_hotMax g_hudOn g_injecting
  g_injectNext g_injects g_iter g_jumpBuf
  g_keepSnapPos g_keyDown g_killId g_killLog
  g_killOverride g_killTick g_killUid g_killVetoIter
  g_killY g_labelH g_labelW g_lagAtTick
  g_lagFired g_lagMs g_lastDeath g_lastDeathX
  g_lastTailSolved g_lastTimeout g_lastTry g_leafDeadAt
  g_leafVerified g_levels g_live g_mapPref
  g_maskExHi g_maskExLo g_maskHi g_maskLo
  g_maxDoomed g_maxMoving g_maxPlayYLive g_maxStreak
  g_maxY g_maxYAuto g_memAt g_missedPortalMode
  g_missedPortalX g_mode g_modePortals g_mu
  g_musicHeld g_musicPaused g_needTrigDropped g_needTrigSuspect
  g_needUnseen g_nextSnap g_noKill g_nowTick
  g_off g_overlay g_overlayHidden g_pad
  g_panel g_pauseAtX g_pausedForSpeed g_pcCalls
  g_pendingLayer g_phantomBands g_phantomHits g_phantomLifted
  g_phantomScale g_phase g_pitch g_plan
  g_planPath g_prevFilter g_prevTerminate g_progressAtStart
  g_progressLevel g_rc g_reasserted g_recordAttempt
  g_recordKind g_recordRequest g_reentrantUpdates g_renderOff
  g_rephase g_resetCalls g_resetNanos g_resignBlocked
  g_restoreProbe g_resultGeneration g_retiredAt g_retireStage
  g_retiring g_retryAfterSec g_retryDone g_rotObjs
  g_run g_running g_savedMusicVol g_savedSfxVol
  g_savedVolume g_secBands g_secReqCap g_secReqHorizon
  g_secReqPending g_secReqStart g_secReqTarget g_secResetMs
  g_secRestMs g_secs g_secSearching g_secStoreMs
  g_seekBarTick g_seekForward g_seekRevive g_seq
  g_serveMode g_serveReset g_serveWait g_settle
  g_shownAt g_showRequest g_shrinks g_silenced
  g_skipExtras g_sliceMs g_sliceStart g_slots
  g_slowmo g_snapCmp g_snapCollideObj g_snapMode
  g_snapOn g_snapProbe g_snapReps g_snaps
  g_snapVerified g_solidIdx g_solids g_solveStart
  g_spentAnchors g_spine g_spineNext g_spineOff
  g_spineOn g_src g_stage g_stallResetPending
  g_stallRuns g_startTick g_stepDepth g_stepSpine
  g_stop g_stopAt g_stopFired g_streak
  g_stride g_stripH g_stripW g_stripX
  g_stripY g_syncProbe g_t0 g_table
  g_tailPath g_targetDepth g_targetX g_targetY
  g_targetYDir g_task g_test g_testStage
  g_testT0 g_thread g_total g_triedMissedPortal
  g_trigTrace g_uiMode g_unfocusPauseBlocked g_verify
  g_verifyEvery g_verifyTol g_visAVs g_visitAVs
  g_visitDraws g_visitSkips g_visRefreshOn g_visResetPending
  g_vq g_vy g_waited g_warnedNoCkpt
  g_watchAfterSec g_watchAtTick g_watchAtX g_watchBackTicks
  g_watchCycleLast g_watchCycleSec g_watchFlips g_watchPurge
  g_watchStartTick g_winShift g_worldDiff g_xHist
  g_xq g_yq
"""


# ---------------------------------------------------------------------------
def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--root", default=None,
                    help="repo root to audit (default: this script's repo)")
    ap.add_argument("--list", action="store_true",
                    help="print every global with its verdict")
    ap.add_argument("--writers", metavar="RE",
                    help="print every assignment to globals matching RE and "
                         "exit (the evidence behind a bucket)")
    a = ap.parse_args()
    base = Path(a.root).resolve() if a.root else Path(__file__).resolve().parents[1]
    load_allowlist()

    rc = 0
    seen_names = set()
    ratchet = {n for n in RATCHET.split() if not n.startswith("#")}

    # ---- self-check ------------------------------------------------------
    # Every `g_` token in the tree must resolve to a declaration the scanner
    # found, or be listed in NOT_GLOBALS as something else (a local, a struct
    # member, a macro parameter). A declaration form the scanner cannot parse
    # otherwise disappears in silence -- which is how the four g_refKid*
    # arrays sat outside the audit -- and "0 problems" from an instrument that
    # matched nothing is the failure mode this whole file exists to avoid.
    declared, tokens = set(), set()
    for _, sub, _, _ in SCOPES:
        root = base / sub
        if root.is_dir():
            declared |= set(scan_globals(root, base))
            for p in sorted(root.rglob("*")):
                if p.is_file() and p.suffix in SUFFIXES:
                    tokens |= set(re.findall(
                        r"\b" + NAME, strip_directives(strip_comments(
                            p.read_text(encoding="utf-8", errors="replace")))))
    unknown = sorted(tokens - declared - NOT_GLOBALS)
    if unknown:
        rc = 1
        print(f"!! self-check: {len(unknown)} `g_` name(s) used but never "
              f"matched as a declaration. Either the scanner cannot parse "
              f"their declaration form -- fix it, do not paper over it -- or "
              f"they are not globals, in which case add them to NOT_GLOBALS:")
        for u in unknown:
            print(f"      {u}")
        print()

    for scope, sub, entries, mode in SCOPES:
        root = base / sub
        if not root.is_dir():
            print(f"!! {scope}: {root} missing")
            rc = 1
            continue
        g = scan_globals(root, base)
        bodies = collect_bodies(root)
        for e in entries:
            if ("", e) not in bodies:
                print(f"!! {scope}: reset entry point {e}() not found -- the "
                      f"audit would report everything as unreset")
                rc = 1
        done, touched = reset_set(bodies, entries, set(g))
        done |= {n for n in g if n in MEMBERWISE}
        missing = sorted(set(g) - done)
        seen_names |= set(g)
        uses = find_uses(base, set(missing))

        if a.writers:
            for n in sorted(missing):
                if re.search(a.writers, n):
                    print(f"{n}  ({g[n]['file']}:{g[n]['line']})")
                    for where, line, mut in uses[n] or [("(none)", "", 0)]:
                        print(f"    {'W' if mut else 'r'} {where}  {line}")
            continue

        # The one bucket that is a proof rather than a judgement: the name
        # occurs nowhere outside its own declaration, so nothing writes it.
        const = {n for n in missing if not uses[n]}

        print(f"== {scope}: {len(g)} globals under {sub}/, "
              f"{len(done)} reset by {', '.join(entries)}(), "
              f"{len(missing)} not "
              f"({len(const)} of those used nowhere at all)")
        if touched:
            print(f"   note: named in a reset body but never mutated there "
                  f"(not coverage): {', '.join(sorted(touched))}")

        known = ALLOW if mode == "allowlist" else \
            {n: ("ratchet", "carried from the 2026-09-04 baseline, "
                            "not individually classified") for n in ratchet}
        unaccounted = [n for n in missing if n not in known and n not in const]
        findings = [n for n in missing
                    if known.get(n, ("?",))[0] not in SAFE_BUCKETS
                    and n in known and mode == "allowlist"]

        if a.list:
            for n in missing:
                b, why = ("C", "never assigned") if n in const else \
                    known.get(n, ("?", "UNACCOUNTED"))
                i = g[n]
                print(f"   [{b}] {i['qual']:<34} {i['file']}:{i['line']}  "
                      f"{i['type']} = {i['init'] or '{}'}   {why}")

        if findings:
            rc = 1
            print(f"\n   -- {len(findings)} PER-INVOCATION GLOBAL(S) THAT "
                  f"NOTHING CLEARS. In a --one-session cold run these carry "
                  f"level N's value into level N+1:\n")
            for n in findings:
                i = g[n]
                print(f"      [{known[n][0]}] {i['file']}:{i['line']}  "
                      f"{i['qual']}")
                print(f"          {known[n][1]}")
            print()

        if unaccounted:
            rc = 1
            what = ("Decide the bucket and add a line to ALLOWLIST_DATA, or "
                    "add a line to the reset"
                    if mode == "allowlist" else
                    "A global has landed since the baseline. Add a line to "
                    "resetSessionState (or to the right per-lifetime reset), "
                    "or add the name to RATCHET with a reason")
            print(f"\n   !! {len(unaccounted)} global(s) neither reset nor "
                  f"accounted for. {what}:\n")
            for n in unaccounted:
                i = g[n]
                print(f"      {i['file']}:{i['line']}  {i['qual']}  "
                      f"({i['type']} = {i['init'] or '{}'})")
            print()

    # A name in the allowlist or the ratchet that no longer exists means the
    # table has drifted from the code -- and a drifted table is one that has
    # stopped being read, so it fails too rather than rotting quietly.
    stale = sorted((set(ALLOW) | ratchet) - seen_names)
    if stale:
        rc = 1
        print(f"!! {len(stale)} allowlist/ratchet entries name globals that no "
              f"longer exist (or moved out of scope). Delete them: "
              f"{', '.join(stale)}")

    print("OK" if rc == 0 else "FAIL")
    return rc


if __name__ == "__main__":
    sys.exit(main())
