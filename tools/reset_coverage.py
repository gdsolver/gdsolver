r"""Which `g_*` global carries state out of one solve and into the next?

`dp::cliMain` was written as a process: parse a command line, load a level,
search, print, exit. Under that assumption a namespace-scope global IS
per-solve state and nothing ever had to clear one. The mod broke it -- it calls
cliMain in-process dozens of times per level (dp/src/dpcore.cpp), and the
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
  * homonyms.   A name can be declared in several namespaces (g_on is in
                four). Keyed by the bare name, the first declaration was the
                only one in the table, and a reset of ANY of them counted for
                it: padtrace::reset clearing padtrace::g_lines stood for
                orbtrace::g_lines, which nothing clears, and solver::g_killY sat
                outside the audit behind itermap::g_killY. Globals are keyed by
                qualified name, and every token is resolved to the global it
                names from where it stands (Resolver); a token it cannot settle
                covers nothing and is printed.

Residual risk in that resolution: a using-directive at file scope in a
header reaches every header after it in the translation unit, which a file
at a time cannot see, so the tree's file-scope directives (`using namespace
p1;`) are applied everywhere, after the enclosing namespaces and the local
directives. A namespace alias would not be followed; there is none today.

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

Qualified names (2026-10-09) were checked the same two ways. The known
answer holds: 8f1ae6b against 9ceaabe is exactly the same three more. And on
the tree they landed in, every global the bare-name audit judged keeps its
verdict, but for two kinds of change: 33 globals it had not judged at all, or
had counted as reset by a homonym's reset, are now judged, and p1::suite::g_at
reads as written, through `++suite::g_at`, which the bare-name prefix pattern
could not see.

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
    #
    # The names that landed after the baseline are classified one by one in
    # MOD_ALLOWLIST_DATA, each with the reset that governs it, and an A there
    # is a finding as it is in dp/.
    ("mod", "src", ["resetSessionState", "dpsolve::start"], "ratchet"),
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
    """qualified name -> dict(name, file, line, ns, type, init, qual).
    Namespace-scope only.

    Keyed by the QUALIFIED name. Keyed by the bare one, the first declaration
    of a name won and a second one in another namespace was not in the table
    at all: solver::g_killY sat outside the audit behind itermap::g_killY."""
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
                # ...but a `;` INSIDE an initialiser is the initialiser's own: a global built by a
                # lambda (`inline std::array<int, kTouchBits> g_touchFireT = [] { ...; return a;
                # }();`, dynamics.hpp) restarted the statement mid-body, and the `;` that ends the
                # declaration then saw only `}()`. Three of dp's globals vanished that way.
                if not any(k == "init" for k, _ in stack):
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
        # ...with any number of extents: `inline long long g_frameRevCall[4][2] = {...}` (bands.hpp).
        m = re.search(r"\b(" + NAME + r")\s*(?:\[[^\]]*\]\s*)*(?:\{.*\})?\s*$",
                      lhs, re.S)
        # ...or a FUNCTION POINTER, whose name sits inside its declarator:
        # `inline bool (*g_cpAhead)(long long now, long long target) = nullptr;` (itermap.hpp).
        # Five hooks the mod installs at start-up were outside the audit in that form.
        fptr = None if m else re.search(r"\(\s*\*\s*(" + NAME + r")\s*\)\s*\(.*\)\s*$",
                                        lhs, re.S)
        if fptr:
            m = fptr
        if m:
            name = m.group(1)
            ty = " ".join(lhs[:m.start(1)].split())
            if fptr:
                ty = " ".join((lhs[:m.start(1)] + lhs[m.end(1):]).split())
            ty = re.sub(r"^(?:inline|static|constexpr|extern)\s+", "",
                        ty).strip()
            # Only the FIRST declarator carries the type; later ones in
            # `inline size_t g_rotSeen = 0, g_rotRouted = 0;` inherit it.
            if ty0 is None:
                ty0 = ty
            elif not ty:
                ty = ty0
            qual = f"{ns}::{name}" if ns else name
            if qual not in found:         # first declaration wins
                init = seg[len(lhs) + 1:].strip() if "=" in seg else ""
                # line number from the NAME's absolute offset, not the
                # statement's: `a` sits just past the previous `;`, i.e. on the
                # previous line, which reported every global one line early.
                pos = a + off + m.start(1)
                found[qual] = dict(
                    name=name, file=rel, line=text.count("\n", 0, pos) + 1, ns=ns,
                    type=ty or ty0 or "?", init=" ".join(init.split())[:60],
                    qual=qual)
        off += len(seg) + 1               # +1 for the comma split_top removed


# ---------------------------------------------------------------------------
# Reset coverage: transitive closure from the entry points
# ---------------------------------------------------------------------------
FUNC = re.compile(r"\binline\s+(?:void|bool|int)\s+(\w+)\s*\([^;{]*\)\s*\{")

# What a write looks like, anchored at the token (the match starts at its
# qualifier): `_writes` tests these just after it, and a prefix `++`/`--` or a
# range-for colon just before it. Anchored, one statement's write to a name
# cannot make another occurrence of it on the same line read as written.
_W_AFTER = [
    # plain / subscripted assignment, but never `==`; and the compound forms.
    # Any number of subscripts: `g_frameRevCall[i][j] = g_frameRevHit[i][j] = 0;` (reset.hpp).
    re.compile(r"\s*(?:\[[^\]]*\]\s*)*(?:[-+*/|&^%]|<<|>>)?=(?!=)"),
    # `g_rotSeen++`. `++g_rotSeen` and `++g_fireBNoTick` were once reported as
    # reads, which made two accumulating counters look like constants; the
    # prefix form is tested before the token, and with the qualifier inside
    # the match `++suite::g_at` is seen too, which a bare-name pattern was not.
    re.compile(r"\s*(?:\+\+|--)"),
    # the container idioms reset.hpp uses
    re.compile(r"\s*\.\s*(?:clear|reset|fill|assign|close|erase|resize|swap|"
               r"pop_back)\s*\("),
    # an atomic's writes, element by element too: reset.hpp clears g_formulaRecHit with
    # `.store(0)` and g_keyCount / g_countSaid with `g_keyCount[b].store(0)` in a loop, and all four
    # were reported as "named in a reset body but never mutated there".
    re.compile(r"\s*(?:\[[^\]]*\])?\s*\.\s*(?:store|exchange|fetch_add|"
               r"fetch_sub|fetch_or|fetch_and)\s*\("),
]


def namespace_spans(text: str) -> list:
    """(open, close, name) for every `namespace X {` in `text`, brace-matched.

    A function belongs to the innermost namespace whose span CONTAINS it. Taking
    the last `namespace X {` written before the function instead is wrong as soon
    as a namespace opens and closes in between: repair.hpp's `namespace rotseed`
    (a few helpers, closed again) sits between `namespace dpsolve {` and
    dpsolve::start(), so start() was filed under rotseed and the audit reported
    its entry point as not found -- and every reset inside it as missing."""
    spans = []
    for nm in re.finditer(r"\bnamespace\s+([\w:]+)\s*\{", text):
        i, depth = nm.end() - 1, 0
        while i < len(text):
            if text[i] == "{":
                depth += 1
            elif text[i] == "}":
                depth -= 1
                if depth == 0:
                    break
            i += 1
        spans.append((nm.start(), i, nm.group(1)))
    return spans


# ---------------------------------------------------------------------------
# Which global a token names
# ---------------------------------------------------------------------------
# A token is an optional qualifier and a g_ name: `g_x`, `secsolve::g_x`,
# `::p1::g_x`. It names a global the way C++ lookup would find it from where it
# stands: the qualifier relative to each enclosing namespace, innermost first;
# then through the using-directives in force there (`using namespace secsolve;`
# opens runSectionVerify, `using namespace p1;` most .cpp files); and only then,
# as a last resort, any global whose qualified name ends in what was written.
# Several answers at that last step is AMBIGUOUS, and an ambiguous token covers
# nothing: matching on the bare name is what let a reset of itermap::g_killY
# stand for solver::g_killY.
QTOK = re.compile(r"(?<![\w:])((?:::\s*)?(?:\w+\s*::\s*)*)(" + NAME + r")\b")
USING = re.compile(r"\busing\s+namespace\s+((?:::)?[\w:]+)\s*;")


class FileCtx:
    """One file's namespaces and using-directives, by offset."""

    def __init__(self, text: str):
        self.text = text
        self.spans = namespace_spans(text)
        opens, pairs = [], []
        for i, ch in enumerate(text):
            if ch == "{":
                opens.append(i)
            elif ch == "}" and opens:
                pairs.append((opens.pop(), i))
        pairs.sort()
        self.usings = []
        for m in USING.finditer(text):
            end = len(text)            # file scope: to the end of the file
            for o, c in pairs:         # ...else the innermost block holding it
                if o < m.start() < c:
                    end = c
                elif o > m.start():
                    break
            self.usings.append((m.end(), end, m.group(1).lstrip(":")))
        self.file_usings = [u for a, e, u in self.usings if e == len(text)]

    def ns_at(self, pos: int) -> str:
        return "::".join(n for o, c, n in self.spans if o < pos < c)

    def usings_at(self, pos: int) -> list:
        """Innermost first: a later directive is in a narrower block, or the same."""
        return [u for a, e, u in reversed(self.usings) if a <= pos < e]


def _prefixes(ns: str) -> list:
    parts = [p for p in ns.split("::") if p]
    return ["::".join(parts[:k]) for k in range(len(parts), -1, -1)]


def _join(*xs) -> str:
    return "::".join(x for x in xs if x)


class Resolver:
    """Token -> the qualified globals it can mean, over every scanned global."""

    def __init__(self, quals, global_usings=()):
        self.quals = set(quals)
        self.by_name: dict[str, list] = {}
        for q in self.quals:
            self.by_name.setdefault(q.rsplit("::", 1)[-1], []).append(q)
        self.global_usings = list(global_usings)

    def resolve(self, qualifier: str, name: str, ns: str, usings: list) -> list:
        q = re.sub(r"\s+", "", qualifier)
        if q.startswith("::"):
            c = _join(q.strip(":"), name)
            return [c] if c in self.quals else []
        q = q.strip(":")
        for p in _prefixes(ns):
            c = _join(p, q, name)
            if c in self.quals:
                return [c]
        for u in list(usings) + self.global_usings:
            for p in _prefixes(ns):
                c = _join(p, u, q, name)
                if c in self.quals:
                    return [c]
        tail = _join(q, name)
        return sorted(x for x in self.by_name.get(name, ())
                      if x == tail or x.endswith("::" + tail))


def _writes(text: str, start: int, end: int) -> bool:
    after = text[end:end + 200]
    if any(p.match(after) for p in _W_AFTER):
        return True
    before = text[max(0, start - 16):start]
    if re.search(r"(?:\+\+|--)\s*$", before):
        return True
    # a range-for subject: `for (auto& f : g_frameLv) f.reset();`
    return bool(re.search(r":\s*$", before) and not before.endswith("::")
                and re.match(r"\s*\)", after))


def tokens_in(ctx: FileCtx, lo: int, hi: int, resolver: "Resolver"):
    """(qualified candidates, is a write, offset, bare name) for every g_ token
    in text[lo:hi]."""
    for m in QTOK.finditer(ctx.text, lo, hi):
        ns = ctx.ns_at(m.start())
        us = ctx.usings_at(m.start())
        yield (resolver.resolve(m.group(1), m.group(2), ns, us),
               _writes(ctx.text, m.start(), m.end()), m.start(), m.group(2))


def collect_bodies(root: Path) -> dict:
    """(namespace, funcname) -> (FileCtx, body start, body end), comments
    stripped. `namespace` is the innermost one, as a call names it."""
    bodies = {}
    for path in sorted(root.rglob("*")):
        if not path.is_file() or path.suffix not in SUFFIXES:
            continue
        text = strip_comments(path.read_text(encoding="utf-8",
                                             errors="replace"))
        ctx = FileCtx(text)
        spans = ctx.spans
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
            body = (ctx, m.end(), i)
            ns, nsOpen = "", -1
            for o, c, name in spans:      # the innermost namespace containing it
                if o < m.start() < c and o > nsOpen:
                    ns, nsOpen = name, o
            bodies.setdefault((ns, m.group(1)), body)
            bodies.setdefault(("", m.group(1)), body)
    return bodies


def entry_key(e: str) -> tuple:
    """An entry point, optionally namespace-qualified as `ns::name`.

    The bare form resolves through the ("", name) slot, which collect_bodies
    fills with setdefault -- so the FIRST file in sorted order that defines that
    name wins it. `start` already has two definitions (repair::start and
    stallwatch::start) and today the right one wins only because "repair.hpp"
    sorts before "stallwatch.hpp". Naming the namespace takes the audit off that
    accident; a file added tomorrow would otherwise silently move the entry
    point, and this script would keep reporting a clean pass for a reset body it
    was no longer reading.
    """
    ns, sep, name = e.rpartition("::")
    return (ns, name) if sep else ("", name)


def reset_set(bodies: dict, entries: list, quals: set, resolver: "Resolver"):
    """Qualified globals mutated by the entry points or anything they call.

    Returns (mutated, touched_only, ambiguous) -- touched_only is globals that
    appear in a reset body but never in a mutating position, which is NOT
    coverage and is worth printing rather than silently counting either way;
    ambiguous is {written token: its candidates} for tokens the lookup could
    not settle, which cover nothing."""
    mutated, touched, seen, ambiguous = set(), set(), set(), {}
    queue = [entry_key(e) for e in entries]
    while queue:
        key = queue.pop()
        if key in seen:
            continue
        seen.add(key)
        got = bodies.get(key)
        if got is None:
            continue
        ctx, lo, hi = got
        # Tokens, not a search per name: a substring test made g_at "touched"
        # by every `g_attempt`, g_fix by `g_fixups`, g_total by
        # `g_totalAttempts` -- ten phantom entries in the report.
        for cands, wr, _, name in tokens_in(ctx, lo, hi, resolver):
            hit = [c for c in cands if c in quals]
            if len(cands) > 1:
                if hit:
                    ambiguous.setdefault(name, set()).update(cands)
                continue
            for c in hit:
                touched.add(c)
                if wr:
                    mutated.add(c)
        body = ctx.text[lo:hi]
        for cm in re.finditer(r"(?:(\w+)\s*::\s*)?(\w+)\s*\(", body):
            queue.append((cm.group(1) or "", cm.group(2)))
    return mutated, touched - mutated, ambiguous


def find_uses(base: Path, names: set, resolver: "Resolver") -> dict:
    """name -> [(file:line, statement, mutating?)] for every occurrence outside
    its own declaration, across the WHOLE tree.

    Two lessons are baked in here.

    Scope: this used to search only the scope's own root, which reported
    dp::g_levelCsv as never assigned -- its writer was src/mod/dp_bridge.cpp,
    in the other root. A global's writers are wherever they are.

    Reads count. The first cut called a global constant when no write pattern
    matched, and that is not a proof: dp::g_progress is only ever mutated
    through `.begin()` / `.end()` / `.layer()`, none of which any assignment
    pattern can see, and it came back "provably never assigned". So the only
    thing claimed automatically now is the honest one -- the name occurs
    NOWHERE outside its declaration, so nothing writes it and nothing reads it.
    Everything else is a judgement and needs a line in the allowlist.

    `names` are qualified, and each occurrence is resolved to the global it
    names (Resolver); a token that could mean several counts for each of them,
    so a use is never lost to an ambiguity -- the safe direction here, where a
    missing use could turn into a false `never assigned`."""
    out = {n: [] for n in names}
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
        if "g_" not in text:
            continue
        lines = text.splitlines()
        rel = str(path.relative_to(base)).replace("\\", "/")
        ctx = FileCtx(text)
        for cands, wr, pos, name in tokens_in(ctx, 0, len(text), resolver):
            hit = [c for c in cands if c in out]
            if not hit:
                continue
            ln = text.count("\n", 0, pos) + 1
            line = lines[ln - 1].strip() if ln <= len(lines) else ""
            # its own declaration is not a use
            if re.match(r"^\s*(?:inline|static|thread_local|constexpr|"
                        r"extern)\b", line) and re.search(
                            r"\b" + name + r"\b\s*(?:\[[^\]]*\])?\s*(?:=|,|;)",
                            line):
                continue
            for c in hit:
                out[c].append((f"{rel}:{ln}", line[:110], wr))
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
#
# The mod's table (MOD_ALLOWLIST_DATA) uses the same letters for the same
# claims, B widened to the mod's lifetimes (the reset that starts the global's
# own lifetime, or the marker its every reader tests first), and one more:
#   K  a cfg key: written ONLY by the autorun.cfg parsers (loadConfig and the
#      parsers it calls), which a suite runs again on the same file before
#      every level. The file does not change within a process, so neither does
#      the value. Not safe the day something rewrites autorun.cfg between two
#      levels of one game, which is the case K would then have to be split by.
# ---------------------------------------------------------------------------
ALLOW: dict[str, tuple] = {}
MOD_ALLOW: dict[str, tuple] = {}


def load_allowlist():
    """Populated from ALLOWLIST_DATA and MOD_ALLOWLIST_DATA below; kept as data
    so the tables read as tables."""
    for data, table in ((ALLOWLIST_DATA, ALLOW), (MOD_ALLOWLIST_DATA, MOD_ALLOW)):
        for line in data.strip().splitlines():
            line = line.strip()
            if not line or line.startswith("#"):
                continue
            bucket, name, reason = line.split(None, 2)
            table[name] = (bucket, reason)


ALLOWLIST_DATA = """
# bucket  name  reason
#
# ---- dp/ : governed by dp::resetInvocationState (dp/src/dp/reset.hpp) -------
#
# (B) the three deliberate exceptions. Two are documented in reset.hpp itself;
#     the third is cleared by cliMain one line above the reset call.
B g_levelCsv        set unconditionally by dpcore::solveInProcess (dp/src/dpcore.cpp) before every in-process cliMain and cleared after it -- the caller's data, not the last solve's (reset.hpp documents it)
B g_progress        a live readout the mod polls from another thread; cliMain re-opens it with g_progress.begin() at cli.hpp:2398 and a ProgressGuard ends it (reset.hpp documents it)
B g_outcome         cleared by cliMain itself at cli.hpp:41, deliberately ABOVE resetInvocationState so an early return leaves "FAILED, nothing measured" rather than the last verdict
B g_plainElsewhere  the caller's hooks, not a solve's state: set unconditionally by dpbridge::plainBeside before every in-process call (src/mod/repair.hpp timedSolve), and never written by dp
B g_ladderBestT     one ladder's: cliMain stores -1 at every ladder's start and its EndLadder guard stores -1 on every way out; outside a ladder nothing reads it
B g_ladderStop      one ladder's: cliMain stores false at every ladder's start, after taking the plain search, and in its EndLadder guard on every way out
B g_wrc             one ladder's walk-resume cache (--walkresume): cliMain stores an empty one at every ladder's start (cli.hpp:7513) and its EndLadder guard clears it (cli.hpp:7525); outside a ladder nothing reads it
B g_walkResumeCheck stored false at the top of every cliMainOnce (cli.hpp:529), before the argument loop that may set it
C g_walkResumeEvery the walk-resume checkpoint stride, 256 ticks; never written
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
# (B) the spider press probe's result, thread_local: stepOne stores false at step.hpp:4031, immediately before
#     the one nested probe step that can set it (the pad pass, step.hpp:15932), and reads it only after that
#     step returns (step.hpp:4038), so no read can see an earlier probe's value.
B g_probePadFired   stored false at step.hpp:4031 before the probe step that alone sets it (step.hpp:15932); read only after it, at step.hpp:4038
#
# ---- classified 2026-10-09: the globals that landed between the 09-04 audit and 10-08 ----------
#
# (B) the second dual half's signals, thread_local: stepBoth sets each one right before the
#     second body's stepOne (fixup.hpp:400-403) and reads or clears it right after (407-410).
B g_bandPortalWrote stored false at fixup.hpp:400 before the second half's stepOne, which alone sets it (step.hpp:14335/14338); read at fixup.hpp:410 right after
B g_dualOtherMode   set for the second half only (fixup.hpp:401, under --dualband) and stored -1 at fixup.hpp:409 right after it; its reader (step.hpp:14312) takes s.mode2 at -1
B g_p2SoloHalf      set at fixup.hpp:402 before the second half and stored false at fixup.hpp:407 right after it
B g_p2SoloFired     stored false at fixup.hpp:403 before the second half, which alone sets it (step.hpp:14192); read at fixup.hpp:408 right after
B g_fxBlockedUid    stored -1 at the top of every stepBoth (fixup.hpp:311), before the fixup lookup that sets it (fixup.hpp:344)
B g_inPressProbe    true only between step.hpp:4030 and 4033, around the one nested probe step
B g_deadKind        written with every `dead = true`: by DIE_AS in the same statement (step.hpp:230) and by the three direct deaths (deadband, maxplayy, offboard, which until 2026-10-09 left it alone); its one reader (--spiderpresslate's test after the probe step) runs only when that step died
#
# (B) set for the length of one call, a ladder or a replay, by the code that opens it.
B g_check           the mod's live channel, not a solve's state: dpcore.cpp:149-176 sets enabled/cancel; cliMain resets it at cli.hpp:5395 when a call uses it, and its CheckGuard on the way out (cli.hpp:5393)
B g_declineToLadder stored true at cli.hpp:7275 around a ladder's first call and false by its Clear guard (cli.hpp:7273/7278)
B g_declinedToLadder stored false at cli.hpp:7276 before the call that alone sets it (cli.hpp:6482); read only after it, at cli.hpp:7279
B g_dynParTasks     lent to the search's pool for one search by DynParLoan (cli.hpp:5432-5435), whose destructor stores nullptr
B g_fileSigHint     the caller's hints, not a solve's state: repair.hpp:2476 sets them before the in-process call and repair.hpp:2487 empties them after it (dp_bridge2.cpp:130-133, dpcore.cpp:36)
B g_groupSigCheck   stored false at the top of every cliMainOnce (cli.hpp:425), before the argument loop that may set it
B g_levelCacheCheck stored false at the top of every cliMainOnce (cli.hpp:426), before the argument loop that may set it
B g_prepMark        stored -1 at the top of every cliMainOnce (cli.hpp:424)
B g_hist            cleared at the start of every replay (cli.hpp:5743); its only reader is that replay's summary (cli.hpp:6302-6305)
B g_histFired       cleared at the start of every replay (cli.hpp:5744), beside g_hist
B g_histPads        cleared at the start of every replay (cli.hpp:5745), beside g_hist
B g_ladder          stored when a ladder starts (cli.hpp:7250) and 0 when it ends (cli.hpp:7255)
B g_ladderLevel     one ladder's level cache, taken only under its own g_ladder (cli.hpp:2640) and cleared when the ladder ends (cli.hpp:7256)
B g_lpc             one ladder's prefix cache, cleared when the ladder starts and when it ends (cli.hpp:7247/7257)
B g_loadPrinted     points at a local only while the level loads (cli.hpp:2660) and is stored nullptr right after (cli.hpp:2664)
B g_tpForceCol      stored at every level load from the objrects header (level_loader.hpp:363); a cached level is taken only when it was the last one loaded (the g_replayLevel / g_ladderLevel keys)
#
# (B) a payload read only below its own counter, which reset.hpp zeroes.
B g_fixupRotX       the payload of g_fixupRotSeen (reset.hpp:309); the only reader prints indices below it (cli.hpp:6587)
B g_fixupCallRotX   the payload of g_fixupCallRotSeen (reset.hpp:310); the only reader prints indices below it (cli.hpp:6581)
B g_impTrail        the ring of g_impCount (reset.hpp:96, step.hpp:2060); the only reader prints the last min(g_impCount, 8) (cli.hpp:6238-6241)
B g_vpTrail         the ring of g_vpWrites (reset.hpp:95, step.hpp:2059); the only reader prints the last min(g_vpWrites, 8) (cli.hpp:6233-6236)
B g_rotWatchUid     written only under --rotwatch (g_rotWatchLo >= 0, which reset.hpp:326 stores -1; step.hpp:918/1108), and model.hpp:386 stores -1 before the read at model.hpp:392
#
# (C) constants held as globals.
C g_ladderPrefixEvery    never written (search_dp.hpp:722)
C g_ladderPrefixMaxSnaps never written (search_dp.hpp:723)
#
# (S) process lifetime by design.
S g_envKills        a running total by design: the mod reads it through dpcore envKillsTotal() and takes differences across a span (dp_bridge.cpp:97/133)
S g_groupLayersGen  a generation number for the groups cache, bumped whenever the layers change (cli.hpp:179); the level caches compare against it, so it must outlive a call
S g_levelTextCache  a cross-call cache by design: taken only while the level text is the same text (cli.hpp:72); emptied between calls by releaseCrossCallCaches (cli.hpp:416), the mod's call when a solve is over
S g_groupLayersCache a cross-call cache by design: taken only on the same files by signature or bytes and the same hold setting (cli.hpp:133-176), rebuilt and its generation bumped otherwise (cli.hpp:237); emptied by releaseCrossCallCaches (cli.hpp:416)
S g_trigRowsCache   a cross-call cache by design: taken only while the file holds the bytes it was parsed from (triggers.hpp:805); emptied by releaseCrossCallCaches (cli.hpp:416)
S g_groupMembersCache a cross-call cache by design: taken only while the file holds the bytes it was parsed from (triggers.hpp:827); emptied by releaseCrossCallCaches (cli.hpp:416)
S g_replayLevel     a cross-call cache by design: taken only when the args, the groups generation, the csv and the files all match (cli.hpp:2645-2647), and cleared by any call that is not a replay (cli.hpp:2620)
S g_run             a level's run spans many calls by design: the mod opens and closes it (runBegin / runEnd, search_dp.hpp:2874/2965)
S g_threadSlot      a worker thread's identity, stored once when the pool's thread starts (thread_pool.hpp:15)
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

MOD_ALLOWLIST_DATA = """
# bucket  name  reason
#
# ---- src/ : the globals that landed after the 2026-09-04 ratchet, classified 2026-10-09 ----
#
# (K) cfg keys, written only by the autorun.cfg parsers (session.hpp loadConfig and the
#     parsers it calls; a suite runs it again on the same file before every level,
#     level_entry.hpp:229-230). Checked: no other writer anywhere under src/.
K g_actKey               cfg key: written only by the autorun.cfg parsers, which a suite runs again on the same file before every level (level_entry.hpp:229-230)
K g_areaT0               cfg key: written only by the autorun.cfg parsers, which a suite runs again on the same file before every level (level_entry.hpp:229-230)
K g_areaT1               cfg key: written only by the autorun.cfg parsers, which a suite runs again on the same file before every level (level_entry.hpp:229-230)
K g_areaUids             cfg key: written only by the autorun.cfg parsers, which a suite runs again on the same file before every level (level_entry.hpp:229-230)
K g_attemptOne           cfg key: written only by the autorun.cfg parsers, which a suite runs again on the same file before every level (level_entry.hpp:229-230)
K g_attemptCount         cfg key: written only by the autorun.cfg parsers, which a suite runs again on the same file before every level (level_entry.hpp:229-230)
K g_attemptAuto          cfg key: written only by the autorun.cfg parsers, which a suite runs again on the same file before every level (level_entry.hpp:229-230)
K g_camRand             cfg key: written only by the autorun.cfg parsers, which a suite runs again on the same file before every level (level_entry.hpp:229-230)
K g_vtime                cfg key: written only by the autorun.cfg parsers, which a suite runs again on the same file before every level (level_entry.hpp:229-230)
K g_replayStepDiagDt     cfg key: written only by the autorun.cfg parsers, which a suite runs again on the same file before every level (level_entry.hpp:229-230)
K g_replayStepCfg       cfg key: written only by the autorun.cfg parsers, which a suite runs again on the same file before every level (level_entry.hpp:229-230)
K g_replayStep          cfg key: written only by the autorun.cfg parsers, which a suite runs again on the same file before every level (level_entry.hpp:229-230)
K g_stopFollowers       cfg key: written only by the autorun.cfg parsers, which a suite runs again on the same file before every level (level_entry.hpp:229-230)
K g_vtScale             cfg key: written only by the autorun.cfg parsers, which a suite runs again on the same file before every level (level_entry.hpp:229-230)
K g_nodeDbg0            cfg key: written only by the autorun.cfg parsers, which a suite runs again on the same file before every level (level_entry.hpp:229-230)
K g_nodeDbg1            cfg key: written only by the autorun.cfg parsers, which a suite runs again on the same file before every level (level_entry.hpp:229-230)
K g_rotDbg0              cfg key: written only by the autorun.cfg parsers, which a suite runs again on the same file before every level (level_entry.hpp:229-230)
K g_rotDbg1              cfg key: written only by the autorun.cfg parsers, which a suite runs again on the same file before every level (level_entry.hpp:229-230)
K g_batchFrom            cfg key: written only by the autorun.cfg parsers, which a suite runs again on the same file before every level (level_entry.hpp:229-230)
K g_batchTo              cfg key: written only by the autorun.cfg parsers, which a suite runs again on the same file before every level (level_entry.hpp:229-230)
K g_bookFirst            cfg key: written only by the autorun.cfg parsers, which a suite runs again on the same file before every level (level_entry.hpp:229-230)
K g_boundOn              cfg key: written only by the autorun.cfg parsers, which a suite runs again on the same file before every level (level_entry.hpp:229-230)
K g_cover                cfg key: written only by the autorun.cfg parsers, which a suite runs again on the same file before every level (level_entry.hpp:229-230)
K g_cpCheck              cfg key: written only by the autorun.cfg parsers, which a suite runs again on the same file before every level (level_entry.hpp:229-230)
K g_cpPlayer             cfg key: written only by the autorun.cfg parsers, which a suite runs again on the same file before every level (level_entry.hpp:229-230)
K g_driftLog             cfg key: written only by the autorun.cfg parsers, which a suite runs again on the same file before every level (level_entry.hpp:229-230)
K g_graceDash            cfg key: written only by the autorun.cfg parsers, which a suite runs again on the same file before every level (level_entry.hpp:229-230)
K g_graceFly             cfg key: written only by the autorun.cfg parsers, which a suite runs again on the same file before every level (level_entry.hpp:229-230)
K g_groupCheck           cfg key: written only by the autorun.cfg parsers, which a suite runs again on the same file before every level (level_entry.hpp:229-230)
K g_headAccel            cfg key: written only by the autorun.cfg parsers, which a suite runs again on the same file before every level (level_entry.hpp:229-230)
K g_headCheck            cfg key: written only by the autorun.cfg parsers, which a suite runs again on the same file before every level (level_entry.hpp:229-230)
K g_headDash             cfg key: written only by the autorun.cfg parsers, which a suite runs again on the same file before every level (level_entry.hpp:229-230)
K g_jbKeep               cfg key: written only by the autorun.cfg parsers, which a suite runs again on the same file before every level (level_entry.hpp:229-230)
K g_latchAt              cfg key: written only by the autorun.cfg parsers, which a suite runs again on the same file before every level (level_entry.hpp:229-230)
K g_layerInput           cfg key: written only by the autorun.cfg parsers, which a suite runs again on the same file before every level (level_entry.hpp:229-230)
K g_leafLife             cfg key: written only by the autorun.cfg parsers, which a suite runs again on the same file before every level (level_entry.hpp:229-230)
K g_leafMid              cfg key: written only by the autorun.cfg parsers, which a suite runs again on the same file before every level (level_entry.hpp:229-230)
K g_levelDir             cfg key: written only by the autorun.cfg parsers, which a suite runs again on the same file before every level (level_entry.hpp:229-230)
K g_objWatch             cfg key: written only by the autorun.cfg parsers, which a suite runs again on the same file before every level (level_entry.hpp:229-230)
K g_p2Extras             cfg key: written only by the autorun.cfg parsers, which a suite runs again on the same file before every level (level_entry.hpp:229-230)
K g_p2Key                cfg key: written only by the autorun.cfg parsers, which a suite runs again on the same file before every level (level_entry.hpp:229-230)
K g_pauseAtTick          cfg key: written only by the autorun.cfg parsers, which a suite runs again on the same file before every level (level_entry.hpp:229-230)
K g_pressT0              cfg key: written only by the autorun.cfg parsers, which a suite runs again on the same file before every level (level_entry.hpp:229-230)
K g_pressT1              cfg key: written only by the autorun.cfg parsers, which a suite runs again on the same file before every level (level_entry.hpp:229-230)
K g_profSample           cfg key: written only by the autorun.cfg parsers, which a suite runs again on the same file before every level (level_entry.hpp:229-230)
K g_regionAt             cfg key: written only by the autorun.cfg parsers, which a suite runs again on the same file before every level (level_entry.hpp:229-230)
K g_regionBox            cfg key: written only by the autorun.cfg parsers, which a suite runs again on the same file before every level (level_entry.hpp:229-230)
K g_rngFix               cfg key: written only by the autorun.cfg parsers, which a suite runs again on the same file before every level (level_entry.hpp:229-230)
K g_rngFresh             cfg key: written only by the autorun.cfg parsers, which a suite runs again on the same file before every level (level_entry.hpp:229-230)
K g_rngSeedEE0           cfg key: written only by the autorun.cfg parsers, which a suite runs again on the same file before every level (level_entry.hpp:229-230)
K g_rngSeedEF8           cfg key: written only by the autorun.cfg parsers, which a suite runs again on the same file before every level (level_entry.hpp:229-230)
K g_rngSeedSet           cfg key: written only by the autorun.cfg parsers, which a suite runs again on the same file before every level (level_entry.hpp:229-230)
K g_rungCoinOff          cfg key: written only by the autorun.cfg parsers, which a suite runs again on the same file before every level (level_entry.hpp:229-230)
K g_secCoins             cfg key: written only by the autorun.cfg parsers, which a suite runs again on the same file before every level (level_entry.hpp:229-230)
K g_sectionHookLazy      cfg key: written only by the autorun.cfg parsers, which a suite runs again on the same file before every level (level_entry.hpp:229-230)
K g_seekBlind            cfg key: written only by the autorun.cfg parsers, which a suite runs again on the same file before every level (level_entry.hpp:229-230)
K g_seekCpEvery          cfg key: written only by the autorun.cfg parsers, which a suite runs again on the same file before every level (level_entry.hpp:229-230)
K g_seekCpFrom           cfg key: written only by the autorun.cfg parsers, which a suite runs again on the same file before every level (level_entry.hpp:229-230)
K g_seekCpKeep           cfg key: written only by the autorun.cfg parsers, which a suite runs again on the same file before every level (level_entry.hpp:229-230)
K g_seekCpTo             cfg key: written only by the autorun.cfg parsers, which a suite runs again on the same file before every level (level_entry.hpp:229-230)
K g_seekVisEvery         cfg key: written only by the autorun.cfg parsers, which a suite runs again on the same file before every level (level_entry.hpp:229-230)
K g_shaderSig            cfg key: written only by the autorun.cfg parsers, which a suite runs again on the same file before every level (level_entry.hpp:229-230)
K g_shaderSkip           cfg key: written only by the autorun.cfg parsers, which a suite runs again on the same file before every level (level_entry.hpp:229-230)
K g_sizeFam              cfg key: written only by the autorun.cfg parsers, which a suite runs again on the same file before every level (level_entry.hpp:229-230)
K g_slopeT0              cfg key: written only by the autorun.cfg parsers, which a suite runs again on the same file before every level (level_entry.hpp:229-230)
K g_slopeT1              cfg key: written only by the autorun.cfg parsers, which a suite runs again on the same file before every level (level_entry.hpp:229-230)
K g_snapAct              cfg key: written only by the autorun.cfg parsers, which a suite runs again on the same file before every level (level_entry.hpp:229-230)
K g_snapEM               cfg key: written only by the autorun.cfg parsers, which a suite runs again on the same file before every level (level_entry.hpp:229-230)
K g_snapEMFrom           cfg key: written only by the autorun.cfg parsers, which a suite runs again on the same file before every level (level_entry.hpp:229-230)
K g_snapRing             cfg key: written only by the autorun.cfg parsers, which a suite runs again on the same file before every level (level_entry.hpp:229-230)
K g_stickT0              cfg key: written only by the autorun.cfg parsers, which a suite runs again on the same file before every level (level_entry.hpp:229-230)
K g_stickT1              cfg key: written only by the autorun.cfg parsers, which a suite runs again on the same file before every level (level_entry.hpp:229-230)
K g_trailClear           cfg key: written only by the autorun.cfg parsers, which a suite runs again on the same file before every level (level_entry.hpp:229-230)
K g_wsnapAny             cfg key: written only by the autorun.cfg parsers, which a suite runs again on the same file before every level (level_entry.hpp:229-230)
K g_wsnapAuto            cfg key: written only by the autorun.cfg parsers, which a suite runs again on the same file before every level (level_entry.hpp:229-230)
K g_wsnapGate            cfg key: written only by the autorun.cfg parsers, which a suite runs again on the same file before every level (level_entry.hpp:229-230)
K g_wsnapGroups          cfg key: written only by the autorun.cfg parsers, which a suite runs again on the same file before every level (level_entry.hpp:229-230)
K g_wsnapSet             cfg key: written only by the autorun.cfg parsers, which a suite runs again on the same file before every level (level_entry.hpp:229-230)
K clearance::g_on        cfg key: written only by the autorun.cfg parsers, which a suite runs again on the same file before every level (level_entry.hpp:229-230)
K p1::itermap::g_actCensus cfg key: written only by the autorun.cfg parsers, which a suite runs again on the same file before every level (level_entry.hpp:229-230)
K secsolve::g_off        cfg key: written only by the autorun.cfg parsers, which a suite runs again on the same file before every level (level_entry.hpp:229-230)
K secsolve::g_visRefresh cfg key: written only by the autorun.cfg parsers, which a suite runs again on the same file before every level (level_entry.hpp:229-230)
K solver::ckpttrace::g_on cfg key: written only by the autorun.cfg parsers, which a suite runs again on the same file before every level (level_entry.hpp:229-230)
K g_lieSkipPsnap         cfg key: written only by the autorun.cfg parsers, which a suite runs again on the same file before every level (level_entry.hpp:229-230)
#
# (B) reset at the start of their own lifetime -- a section search, a seek, an attempt, a
#     rung, a session's first attempt -- outside the two entry points above; or a payload
#     read only behind a marker that is reset; or a guard set and cleared around one call.
B g_accel2               cleared at every section search's head (hooks_gamelayer.cpp:4282-4284), refilled by secPushP2
B g_actCaptures          cleared by psnap::buildActWindow (psnap.hpp:800-805) at every section search's head (hooks_gamelayer.cpp:4446)
B g_actCapUs             cleared by psnap::buildActWindow (psnap.hpp:800-805) at every section search's head (hooks_gamelayer.cpp:4446)
B g_actDecos             cleared by psnap::buildActWindow (psnap.hpp:800-805) at every section search's head (hooks_gamelayer.cpp:4446)
B g_actObjs              cleared by psnap::buildActWindow (psnap.hpp:800-805) at every section search's head (hooks_gamelayer.cpp:4446)
B g_actRestores          cleared by psnap::buildActWindow (psnap.hpp:800-805) at every section search's head (hooks_gamelayer.cpp:4446)
B g_actRestUs            cleared by psnap::buildActWindow (psnap.hpp:800-805) at every section search's head (hooks_gamelayer.cpp:4446)
B g_actSeen              cleared by psnap::buildActWindow (psnap.hpp:800-805) at every section search's head (hooks_gamelayer.cpp:4446)
B g_areaArr              print only (cfg areatrace): looked up again when the object array changes and at the window's first tick, before the window reads them (hooks_gamelayer.cpp:7501-7509)
B g_areaObjs             print only (cfg areatrace): looked up again when the object array changes and at the window's first tick, before the window reads them (hooks_gamelayer.cpp:7501-7509)
B g_attemptFed           cleared at every attempt's start (hooks_playlayer.cpp:292)
B g_attemptT0            set at every attempt's start (cpflight::onAttemptStart, cp_flight.hpp:910-915)
B g_boundSkips           zeroed at every section search's head (hooks_gamelayer.cpp:4463-4470)
B g_boxX0                set at every section search's head when g_boundOn and the layer has objects (hooks_gamelayer.cpp:4471-4480); read only under g_active && g_boundOn (hooks_gamelayer.cpp:7963)
B g_boxX1                set at every section search's head when g_boundOn and the layer has objects (hooks_gamelayer.cpp:4471-4480); read only under g_active && g_boundOn (hooks_gamelayer.cpp:7963)
B g_boxY0                set at every section search's head when g_boundOn and the layer has objects (hooks_gamelayer.cpp:4471-4480); read only under g_active && g_boundOn (hooks_gamelayer.cpp:7963)
B g_boxY1                set at every section search's head when g_boundOn and the layer has objects (hooks_gamelayer.cpp:4471-4480); read only under g_active && g_boundOn (hooks_gamelayer.cpp:7963)
B g_buf                  assigned by profsample::start() at every sampling run, released by stopAndReport (profsample.hpp:42-53, 90-131)
B g_ckAbandonJob         cfg dpcheck: consumed by the abandoned job's own collect, which poll() runs before its generation check (repair.hpp:8780 vs 8786), so it is taken even when that job reports into the next level's session
B g_ckptDash2            written on both branches whenever a head checkpoint is made (hooks_gamelayer.cpp:991-994)
B g_coinBest             assigned by solver::buildPois (solver.hpp:707-712), with solver::g_coins, once per session (g_poisBuilt; hooks_playlayer.cpp:353-356)
B g_coinLiveSaid         assigned at every attempt's start (hooks_playlayer.cpp:326)
B g_coinMoveX            assigned by solver::buildPois (solver.hpp:707-712), with solver::g_coins, once per session (g_poisBuilt; hooks_playlayer.cpp:353-356)
B g_coinObjs             assigned by solver::buildPois (solver.hpp:707-712), with solver::g_coins, once per session (g_poisBuilt; hooks_playlayer.cpp:353-356)
B g_coinPosLog           assigned by solver::buildPois (solver.hpp:707-712), with solver::g_coins, once per session (g_poisBuilt; hooks_playlayer.cpp:353-356)
B g_compound             areaenv::resetTallies(), from grouptrace::build at a level's first attempt whenever grouptrace is on (grouptrace.hpp:157, hooks_playlayer.cpp:330-332; resetSessionState clears the owner); they move only under envActive(), which needs grouptrace on (hooks_area.cpp:16); areaenv::g_objects too
B g_controlAtT           a probe's and its control's books: written when the probe starts (hooks_gamelayer.cpp:3148), cleared when it is captured and ended (cp_flight.hpp:1370-1372, 1470, 1496); read only under g_probeFlying / g_controlFlying
B g_decoSeen             assigned by buildActWindow past its g_snapAct gate (psnap.hpp:817); with the gate shut it is never filled, and g_snapAct is a cfg key
B g_degenerate           areaenv::resetTallies(), from grouptrace::build at a level's first attempt whenever grouptrace is on (grouptrace.hpp:157, hooks_playlayer.cpp:330-332; resetSessionState clears the owner); they move only under envActive(), which needs grouptrace on (hooks_area.cpp:16); areaenv::g_objects too
B g_diffCollect          print only (cfg cpflightlayerdiff): set at every take and start (hooks_gamelayer.cpp:3058-3060, 3139-3141)
B g_diffCompare          print only (cfg cpflightlayerdiff): set at every take and start (hooks_gamelayer.cpp:3058-3060, 3139-3141)
B g_diffStep             print only (cfg cpflightlayerdiff): set at every take and start (hooks_gamelayer.cpp:3058-3060, 3139-3141)
B g_dpCfgSeen            cleared at every loadConfig (session.hpp:748)
B g_ended                set at every attempt's start (cpflight::onAttemptStart, cp_flight.hpp:910-915)
B g_firstMiss            areaenv::resetTallies(), from grouptrace::build at a level's first attempt whenever grouptrace is on (grouptrace.hpp:157, hooks_playlayer.cpp:330-332; resetSessionState clears the owner); they move only under envActive(), which needs grouptrace on (hooks_area.cpp:16); areaenv::g_objects too
B g_frameGapMs           zeroed at every section search's head (hooks_gamelayer.cpp:4463-4470)
B g_frames               zeroed at every section search's head (hooks_gamelayer.cpp:4463-4470)
B g_headChecked          seekcp::resetSession(), which PlayLayer::onQuit calls before resetSessionState (hooks_playlayer.cpp:170); areaenv::g_objects shares the name (see below)
B g_headJb               captured immediately before every head checkpoint (hooks_gamelayer.cpp:950-956); a section search runs only from that checkpoint
B g_headPlain            captured immediately before every head checkpoint (hooks_gamelayer.cpp:950-956); a section search runs only from that checkpoint
B g_headRun              seekcp::resetSession(), which PlayLayer::onQuit calls before resetSessionState (hooks_playlayer.cpp:170); areaenv::g_objects shares the name (see below)
B g_headX                seekcp::resetSession(), which PlayLayer::onQuit calls before resetSessionState (hooks_playlayer.cpp:170); areaenv::g_objects shares the name (see below)
B g_headY                seekcp::resetSession(), which PlayLayer::onQuit calls before resetSessionState (hooks_playlayer.cpp:170); areaenv::g_objects shares the name (see below)
B g_inPostCollP1         set and cleared around player 1's postCollision (hooks_player.cpp:204-206)
B g_inTake               set and cleared around the checkpoint take (hooks_gamelayer.cpp:2529-2531, 2650-2652)
B g_killsBase            areaenv::resetTallies(), from grouptrace::build at a level's first attempt whenever grouptrace is on (grouptrace.hpp:157, hooks_playlayer.cpp:330-332; resetSessionState clears the owner); they move only under envActive(), which needs grouptrace on (hooks_area.cpp:16); areaenv::g_objects too
B g_lastBatchCalls       zeroed at every batch's start (hooks_gamelayer.cpp:1827); its one reader paces the render-off effect sweep, visual only (hooks_gamelayer.cpp:1779)
B g_layer                seekcp::resetSession(), which PlayLayer::onQuit calls before resetSessionState (hooks_playlayer.cpp:170); areaenv::g_objects shares the name (see below)
B g_lineSig              seekcp::resetSession(), which PlayLayer::onQuit calls before resetSessionState (hooks_playlayer.cpp:170); areaenv::g_objects shares the name (see below)
B g_main                 assigned by profsample::start() at every sampling run, released by stopAndReport (profsample.hpp:42-53, 90-131)
B g_match                areaenv::resetTallies(), from grouptrace::build at a level's first attempt whenever grouptrace is on (grouptrace.hpp:157, hooks_playlayer.cpp:330-332; resetSessionState clears the owner); they move only under envActive(), which needs grouptrace on (hooks_area.cpp:16); areaenv::g_objects too
B g_maxErr               areaenv::resetTallies(), from grouptrace::build at a level's first attempt whenever grouptrace is on (grouptrace.hpp:157, hooks_playlayer.cpp:330-332; resetSessionState clears the owner); they move only under envActive(), which needs grouptrace on (hooks_area.cpp:16); areaenv::g_objects too
B g_memAfter             seekcp::resetSession(), which PlayLayer::onQuit calls before resetSessionState (hooks_playlayer.cpp:170); areaenv::g_objects shares the name (see below)
B g_memBefore            seekcp::resetSession(), which PlayLayer::onQuit calls before resetSessionState (hooks_playlayer.cpp:170); areaenv::g_objects shares the name (see below)
B g_miss                 areaenv::resetTallies(), from grouptrace::build at a level's first attempt whenever grouptrace is on (grouptrace.hpp:157, hooks_playlayer.cpp:330-332; resetSessionState clears the owner); they move only under envActive(), which needs grouptrace on (hooks_area.cpp:16); areaenv::g_objects too
B g_n                    assigned by profsample::start() at every sampling run, released by stopAndReport (profsample.hpp:42-53, 90-131)
B g_nextInputAtStart     written when g_attemptFed flips, on every attempt's first processCommands (hooks_gamelayer.cpp:6550-6553); read by the slice check after a flight (level_slice.hpp:714)
B g_nonFinite            zeroed at every section search's head (hooks_gamelayer.cpp:4613)
B g_nonFiniteId          written on g_nonFinite's 0->1 step (hooks_gamelayer.cpp:7950-7954), read only under g_nonFinite > 0 (hooks_gamelayer.cpp:5147)
B g_nonFiniteUid         written on g_nonFinite's 0->1 step (hooks_gamelayer.cpp:7950-7954), read only under g_nonFinite > 0 (hooks_gamelayer.cpp:5147)
B g_nonFiniteX           written on g_nonFinite's 0->1 step (hooks_gamelayer.cpp:7950-7954), read only under g_nonFinite > 0 (hooks_gamelayer.cpp:5147)
B g_nonFiniteY           written on g_nonFinite's 0->1 step (hooks_gamelayer.cpp:7950-7954), read only under g_nonFinite > 0 (hooks_gamelayer.cpp:5147)
B g_objAfter             print only (cfg cpflightobj, default -1): a 3-update countdown armed at a take or start (hooks_gamelayer.cpp:3049, 3109)
B g_objects              seekcp::resetSession(), which PlayLayer::onQuit calls before resetSessionState (hooks_playlayer.cpp:170); areaenv::g_objects shares the name (see below)
B g_offMiss              areaenv::resetTallies(), from grouptrace::build at a level's first attempt whenever grouptrace is on (grouptrace.hpp:157, hooks_playlayer.cpp:330-332; resetSessionState clears the owner); they move only under envActive(), which needs grouptrace on (hooks_area.cpp:16); areaenv::g_objects too
B g_outside              areaenv::resetTallies(), from grouptrace::build at a level's first attempt whenever grouptrace is on (grouptrace.hpp:157, hooks_playlayer.cpp:330-332; resetSessionState clears the owner); they move only under envActive(), which needs grouptrace on (hooks_area.cpp:16); areaenv::g_objects too
B g_outsideMs            zeroed at every section search's head (hooks_gamelayer.cpp:4463-4470)
B g_overlayMs            zeroed at every section search's head (hooks_gamelayer.cpp:4463-4470)
B g_pad2                 cleared at every section search's head (hooks_gamelayer.cpp:4282-4284), refilled by secPushP2
B g_partSaid             cleared at every attempt's head run (seek_cp.hpp:185) and every seek restore (hooks_gamelayer.cpp:3012)
B g_passing              set and cleared around one call (hooks_playmenu.cpp:540-542, 628-630)
B g_placeholderGroups    assigned from cfg slicekeepgroups before every slice build (level_slice.hpp:217)
B g_postMissCrossT       written with g_coinMissPostPending (repair.hpp:7412-7414), read only when that flag was taken (repair.hpp:7528, 8132); dpsolve::start clears the flag (repair.hpp:6359)
B g_preludePendingAttempt written with g_preludeResetPending (hooks_gamelayer.cpp:6545-6546); the flag is cleared at every attempt's start (hooks_playlayer.cpp:287)
B g_probeAtT             a probe's and its control's books: written when the probe starts (hooks_gamelayer.cpp:3148), cleared when it is captured and ended (cp_flight.hpp:1370-1372, 1470, 1496); read only under g_probeFlying / g_controlFlying
B g_probeCap             a probe's and its control's books: written when the probe starts (hooks_gamelayer.cpp:3148), cleared when it is captured and ended (cp_flight.hpp:1370-1372, 1470, 1496); read only under g_probeFlying / g_controlFlying
B g_rectMiss             areaenv::resetTallies(), from grouptrace::build at a level's first attempt whenever grouptrace is on (grouptrace.hpp:157, hooks_playlayer.cpp:330-332; resetSessionState clears the owner); they move only under envActive(), which needs grouptrace on (hooks_area.cpp:16); areaenv::g_objects too
B g_restoreMs            seekcp::resetSession(), which PlayLayer::onQuit calls before resetSessionState (hooks_playlayer.cpp:170); areaenv::g_objects shares the name (see below)
B g_sampled              areaenv::resetTallies(), from grouptrace::build at a level's first attempt whenever grouptrace is on (grouptrace.hpp:157, hooks_playlayer.cpp:330-332; resetSessionState clears the owner); they move only under envActive(), which needs grouptrace on (hooks_area.cpp:16); areaenv::g_objects too
B g_secPhase             crash breadcrumbs: written before each phase of a section search, g_secPhase back to 0 at its end (hooks_gamelayer.cpp:4907-6125)
B g_secPhaseDepth        crash breadcrumbs: written before each phase of a section search, g_secPhase back to 0 at its end (hooks_gamelayer.cpp:4907-6125)
B g_secPhaseNode         crash breadcrumbs: written before each phase of a section search, g_secPhase back to 0 at its end (hooks_gamelayer.cpp:4907-6125)
B g_secRungFailAttempt   reset at every rung's start (repair.hpp:8687-8688); read only under g_secRung && g_on (repair.hpp:1328)
B g_secRungPrefixFails   reset at every rung's start (repair.hpp:8687-8688); read only under g_secRung && g_on (repair.hpp:1328)
B g_secSaveCkpt          saved at every rung's start (repair.hpp:8672-8695), read only to put the cfg back when that rung ends (repair.hpp:1347-1351, hooks_gamelayer.cpp:6327-6331, session.hpp:241)
B g_secSaveMaxAttempts   saved at every rung's start (repair.hpp:8672-8695), read only to put the cfg back when that rung ends (repair.hpp:1347-1351, hooks_gamelayer.cpp:6327-6331, session.hpp:241)
B g_secSavePractice      saved at every rung's start (repair.hpp:8672-8695), read only to put the cfg back when that rung ends (repair.hpp:1347-1351, hooks_gamelayer.cpp:6327-6331, session.hpp:241)
B g_secSaveTargetDepth   saved at every rung's start (repair.hpp:8672-8695), read only to put the cfg back when that rung ends (repair.hpp:1347-1351, hooks_gamelayer.cpp:6327-6331, session.hpp:241)
B g_secScreenWasOn       written when a render hold begins and read when it is released, under g_secRenderWas (session_hotkeys.hpp:196-209)
B g_seekBatches          reset at every seek's start (itermap.hpp:1327-1331)
B g_seekBlindDone        reset at every seek's start (itermap.hpp:1327-1331)
B g_seekBlindMs          reset at every seek's start (itermap.hpp:1327-1331)
B g_seekBlindNow         reset at every seek's start (itermap.hpp:1327-1331)
B g_seekDrawMs           reset at every seek's start (itermap.hpp:1327-1331)
B g_seekFrames           reset at every seek's start (itermap.hpp:1327-1331)
B g_seekFromCp           interactive only: set with g_seekRestart by the seekfrom command (session.hpp:386), taken and cleared by the restore that restart runs (hooks_gamelayer.cpp:2995)
B g_seekT0               reset at every seek's start (itermap.hpp:1327-1331)
B g_seekVisCalls         reset at every seek's start (itermap.hpp:1327-1331)
B g_seekVisMs            reset at every seek's start (itermap.hpp:1327-1331)
B g_seekVisSkip          set before and cleared after every update of a batch (hooks_gamelayer.cpp:1947-1951)
B g_shaderSigPending     print only (cfg secshadersig): set at a search's end, consumed by the next real reset (hooks_playlayer.cpp:515-519)
B g_shaderSkips          zeroed at every section search's head (hooks_gamelayer.cpp:4463-4470)
B g_sliceEnd             written with g_sliceEnded (hooks_gamelayer.cpp:1515-1516), read only when it is set
B g_sliceEnded           zeroed at every section search's head (hooks_gamelayer.cpp:4463-4470)
B g_snapEMActive         assigned at every section search's head (hooks_gamelayer.cpp:4073)
B g_thinned              seekcp::resetSession(), which PlayLayer::onQuit calls before resetSessionState (hooks_playlayer.cpp:170); areaenv::g_objects shares the name (see below)
B g_thr                  assigned by profsample::start() at every sampling run, released by stopAndReport (profsample.hpp:42-53, 90-131)
B g_tick0                assigned by profsample::start() at every sampling run, released by stopAndReport (profsample.hpp:42-53, 90-131)
B g_toggles              seekcp::resetSession(), which PlayLayer::onQuit calls before resetSessionState (hooks_playlayer.cpp:170); areaenv::g_objects shares the name (see below)
B g_uiArmedLevel         cleared at every level entry, armed or not (hooks_playlayer.cpp:103)
B g_unenveloped          areaenv::resetTallies(), from grouptrace::build at a level's first attempt whenever grouptrace is on (grouptrace.hpp:157, hooks_playlayer.cpp:330-332; resetSessionState clears the owner); they move only under envActive(), which needs grouptrace on (hooks_area.cpp:16); areaenv::g_objects too
B g_valueMiss            areaenv::resetTallies(), from grouptrace::build at a level's first attempt whenever grouptrace is on (grouptrace.hpp:157, hooks_playlayer.cpp:330-332; resetSessionState clears the owner); they move only under envActive(), which needs grouptrace on (hooks_area.cpp:16); areaenv::g_objects too
B g_varying              areaenv::resetTallies(), from grouptrace::build at a level's first attempt whenever grouptrace is on (grouptrace.hpp:157, hooks_playlayer.cpp:330-332; resetSessionState clears the owner); they move only under envActive(), which needs grouptrace on (hooks_area.cpp:16); areaenv::g_objects too
B g_visAvRead            the exception filter's scratch, written just before the line that prints it (playlayer_helpers.hpp:21-47)
B g_visAvRet             the exception filter's scratch, written just before the line that prints it (playlayer_helpers.hpp:21-47)
B g_visAvRip             the exception filter's scratch, written just before the line that prints it (playlayer_helpers.hpp:21-47)
B g_vy2                  cleared at every section search's head (hooks_gamelayer.cpp:4282-4284), refilled by secPushP2
B g_xRemoving            set and cleared around removeCheckpoint (takeover.hpp:229-231)
B areaenv::g_calls       areaenv::resetTallies() with the other tallies (areaenv.hpp:211)
B areaenv::g_objects     areaenv::resetTallies() with the other tallies (areaenv.hpp:211)
B areaenv::g_rows        areaenv::resetTallies() with the other tallies (areaenv.hpp:212)
B p1::profsample::g_t0   assigned by profsample::start() at every sampling run (profsample.hpp:50)
B p1::seekcp::g_checked  seekcp::resetSession(), which PlayLayer::onQuit calls before resetSessionState (hooks_playlayer.cpp:170)
B p1::seekcp::g_cps      seekcp::resetSession(), which PlayLayer::onQuit calls before resetSessionState (hooks_playlayer.cpp:170)
B p1::seekcp::g_from     seekcp::resetSession(), which PlayLayer::onQuit calls before resetSessionState (hooks_playlayer.cpp:170)
B p1::seekcp::g_plan     seekcp::resetSession(), which PlayLayer::onQuit calls before resetSessionState (hooks_playlayer.cpp:170)
B p1::seekcp::g_restores seekcp::resetSession(), which PlayLayer::onQuit calls before resetSessionState (hooks_playlayer.cpp:170)
B p1::seekcp::g_restoring seekcp::resetSession(), which PlayLayer::onQuit calls before resetSessionState (hooks_playlayer.cpp:170)
B p1::seekcp::g_step     seekcp::resetSession(), which PlayLayer::onQuit calls before resetSessionState (hooks_playlayer.cpp:170)
B p1::seekcp::g_takeMs   seekcp::resetSession(), which PlayLayer::onQuit calls before resetSessionState (hooks_playlayer.cpp:170)
B p1::seekcp::g_taken    seekcp::resetSession(), which PlayLayer::onQuit calls before resetSessionState (hooks_playlayer.cpp:170)
#
# (S) process lifetime by design.
S g_actHook              the objwatch hook: resolved once per process (hooks_gamelayer.cpp:312); enabled to match g_objWatch at every sync (hooks_gamelayer.cpp:337-339)
S g_actHookOn            the objwatch hook: resolved once per process (hooks_gamelayer.cpp:312); enabled to match g_objWatch at every sync (hooks_gamelayer.cpp:337-339)
S g_attemptSerial        monotone serial: only copied and compared for equality (repair.hpp:229, 6670)
S g_cfgDefaults          captured once, before the first autorun.cfg is applied (session.hpp:746): what a key nobody wrote runs with
S g_ckMx                 a mutex
S g_gtSerial             file-name serial, unique per process (cp_flight.hpp:1006)
S g_levelDirChecked      the suite's order check, once per suite (level_entry.hpp:21)
S g_nextId               monotone id counter (phase.hpp:87, 134)
S g_remSecHook           the section hooks: resolved once per process (hooks_gamelayer.cpp:345-346), switched by syncSectionHooks from the current mode
S g_remSecOn             the section hooks: resolved once per process (hooks_gamelayer.cpp:345-346), switched by syncSectionHooks from the current mode
S g_rotGameplayCalls     monotone counter, read only as an equality snapshot inside one search (hooks_gamelayer.cpp:4577, 5206)
S g_secHookFlips         print cap: the first four hook switches of the process
S g_secSnapMs            running totals, read only as one search's own difference (hooks_gamelayer.cpp:4935-4936, 5995)
S g_secStepMs            running totals, read only as one search's own difference (hooks_gamelayer.cpp:4935-4936, 5995)
S g_secThrows            print cap: three C++-exception lines per process from the crash filter
S g_secThrowsDone        fault injection (cfg secthrowkind, default 0): the injected throws per process
S g_swapReq              the swaplevel command: its timing spans the swap into the next layer by design (hooks_playlayer.cpp:148-159)
S g_swapT0               the swaplevel command: its timing spans the swap into the next layer by design (hooks_playlayer.cpp:148-159)
S g_swapTiming           the swaplevel command: its timing spans the swap into the next layer by design (hooks_playlayer.cpp:148-159)
S g_uiCoins              the play menu's choices, loaded from the mod's saved values (hooks_playmenu.cpp:591-592): the person's, not a level's
S g_uiGlitch             the play menu's choices, loaded from the mod's saved values (hooks_playmenu.cpp:591-592): the person's, not a level's
S g_updSecHook           the section hooks: resolved once per process (hooks_gamelayer.cpp:345-346), switched by syncSectionHooks from the current mode
S g_updSecOn             the section hooks: resolved once per process (hooks_gamelayer.cpp:345-346), switched by syncSectionHooks from the current mode
S p1::phase::g_mu        a mutex
S p1::stallwatch::g_phase the stall watchdog's view of the main thread: one per process, set by every Mark scope (stallwatch.hpp:28-35)
S p1::stallwatch::g_run  the stall watchdog's thread, started once per process (stallwatch.hpp:40)
#
# (C) not storage of its own.
C g_every                a reference to itermap::g_seekCpEvery (a cfg key)
C clearance::g_mode      never written: declared 1 (clearance.hpp:28), read only
"""

# Buckets that mean "accounted for and safe". Everything else in the allowlist
# is a finding that stays on the report and keeps the exit status non-zero.
SAFE_BUCKETS = {"B", "C", "S", "K"}

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
  g_anchorT g_anchorX g_at
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
  g_pauseAtX g_pausedForSpeed g_pcCalls
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
  g_beside g_besideKills g_checkM g_checkLast g_checked g_differed g_m g_held
  g_plainBesideMode
  g_cpAhead g_cpLine g_onActive g_testGap
  g_snapExtras
"""
# g_snapExtras (src/mod/hooks_gamelayer.cpp) was added 2026-10-09 with MOD_ALLOWLIST_DATA: it is
# index-parallel to g_snaps, a ratchet entry, filled beside it (:926) and read only through a
# g_snaps index (:1307-1309), so it has g_snaps's lifetime and is decided with it.
# The g_cpAhead line above was added 2026-10-09 when the scanner learned to read a function pointer's
# declarator (`inline bool (*g_cpAhead)(...) = nullptr;`): the four were not new, only newly
# visible. All four are hooks the mod installs once and never per session -- itermap::g_cpAhead and
# g_cpLine and secsolve::g_onActive at start-up (hooks_gamelayer.cpp:347-349), phase::g_testGap
# only by tools/phase_fixture.cpp (101/104). The fifth one the fix uncovered, g_cpFlightSummary, is
# set by dpsolve::start().
# The last two lines above were added 2026-09-29 with a reason, as the header asks: process lifetime
# by design, around the second copy of the core. g_beside (src/mod/dp_bridge.cpp) is the plain
# search beside a ladder, every field refilled by its start(); g_besideKills is summed into
# envKillsTotal(), which callers difference across a span; g_checkM, g_checkLast, g_checked and
# g_differed are the dpplainbeside=2 instrument's line (taken after every call) and running
# counts; dp2out::g_m and g_held (dp_bridge2.cpp) hold the second copy's stdout until the search
# it came from is taken or dropped. g_plainBesideMode (src/mod/repair.hpp) is set by
# dpsolve::start() on every session from cfg dpplainbeside or the settings menu.


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
    scanned = {}
    for _, sub, _, _ in SCOPES:
        root = base / sub
        if root.is_dir():
            scanned[sub] = scan_globals(root, base)
            declared |= {i["name"] for i in scanned[sub].values()}
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

    # One resolver over every scanned global, so a token in one root that names
    # a global of the other (dp::g_levelCsv, written by the mod) finds it, and a
    # mod global is never taken for a dp one of the same name. The file-scope
    # using-directives of the tree (`using namespace p1;`) apply everywhere, as
    # they do in a translation unit of chained headers.
    every = {q for d in scanned.values() for q in d}
    file_usings = set()
    for _, sub, _, _ in SCOPES:
        for p in sorted((base / sub).rglob("*")) if (base / sub).is_dir() else ():
            if p.is_file() and p.suffix in SUFFIXES:
                file_usings |= set(FileCtx(strip_comments(p.read_text(
                    encoding="utf-8", errors="replace"))).file_usings)
    resolver = Resolver(every, sorted(file_usings))

    for scope, sub, entries, mode in SCOPES:
        root = base / sub
        if not root.is_dir():
            print(f"!! {scope}: {root} missing")
            rc = 1
            continue
        g = scanned[sub]
        # The bare name a table line uses means the FIRST declaration of it in
        # file order -- the one the bare-name scanner kept, and so the one every
        # line written before qualified names meant. Any other global of that
        # name needs its own line, under its qualified name.
        first = {}
        for q, i in g.items():
            first.setdefault(i["name"], q)
        def key(q, table):
            if q in table:
                return q
            n = g[q]["name"]
            return n if first[n] == q and n in table else None
        seen_names |= set(g) | set(first)
        bodies = collect_bodies(root)
        for e in entries:
            if entry_key(e) not in bodies:
                print(f"!! {scope}: reset entry point {e}() not found -- the "
                      f"audit would report everything as unreset")
                rc = 1
        done, touched, ambiguous = reset_set(bodies, entries, set(g), resolver)
        done |= {q for q in g if q in MEMBERWISE or g[q]["name"] in MEMBERWISE}
        missing = sorted(set(g) - done)
        uses = find_uses(base, set(missing), resolver)

        if a.writers:
            for n in sorted(missing):
                if re.search(a.writers, n) or re.search(a.writers, g[n]["name"]):
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
        if ambiguous:
            print(f"   note: tokens in a reset body that name more than one "
                  f"global, so they cover none: "
                  + "; ".join(f"{n} -> {', '.join(sorted(c))}"
                              for n, c in sorted(ambiguous.items())))

        if mode == "allowlist":
            known = dict(ALLOW)
        else:
            known = {n: ("ratchet", "carried from the 2026-09-04 baseline, "
                                    "not individually classified") for n in ratchet}
            both = sorted(ratchet & set(MOD_ALLOW))
            if both:
                rc = 1
                print(f"!! {scope}: in RATCHET and MOD_ALLOWLIST_DATA both (two "
                      f"verdicts for one name; keep one): {', '.join(both)}")
            known.update(MOD_ALLOW)
        verdict = {q: known[k] for q in missing if (k := key(q, known))}
        unaccounted = [n for n in missing if n not in verdict and n not in const]
        # A ratchet entry is no verdict, so it is neither safe nor a finding.
        findings = [n for n in missing if n in verdict
                    and verdict[n][0] not in SAFE_BUCKETS
                    and verdict[n][0] != "ratchet"]

        if a.list:
            for n in missing:
                b, why = ("C", "never assigned") if n in const else \
                    verdict.get(n, ("?", "UNACCOUNTED"))
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
                print(f"      [{verdict[n][0]}] {i['file']}:{i['line']}  "
                      f"{i['qual']}")
                print(f"          {verdict[n][1]}")
            print()

        if unaccounted:
            rc = 1
            what = ("Decide the bucket and add a line to ALLOWLIST_DATA, or "
                    "add a line to the reset"
                    if mode == "allowlist" else
                    "A global has landed since the baseline. Add a line to "
                    "resetSessionState (or to the right per-lifetime reset), "
                    "or decide its bucket in MOD_ALLOWLIST_DATA")
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
    stale = sorted((set(ALLOW) | set(MOD_ALLOW) | ratchet) - seen_names)
    if stale:
        rc = 1
        print(f"!! {len(stale)} allowlist/ratchet entries name globals that no "
              f"longer exist (or moved out of scope). Delete them: "
              f"{', '.join(stale)}")

    print("OK" if rc == 0 else "FAIL")
    return rc


if __name__ == "__main__":
    sys.exit(main())
