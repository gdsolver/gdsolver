#!/usr/bin/env python
"""Generate the two functions that print what a session actually runs with.

    python tools/gen_effective_cfg.py            # rewrite the generated headers
    python tools/gen_effective_cfg.py --check    # exit 1 if they are stale

A cold run's manifest used to record the cfg the driver WROTE and the first solver call's
flags. Neither shows a default: a mod key nobody wrote, or a dp global the call did not name,
is invisible in both, so "dpmodelreanchor was on" could only be asserted, not read. These two
functions print the values themselves, read from the variables at run time:

  src/mod/cfg_effective.gen.hpp     p1::effcfg::modCfg() -- every autorun.cfg key the mod's
                                    parser (loadConfig / loadDpCfg / loadTraceCfg in
                                    src/mod/session.hpp) knows, with the value of the
                                    variable(s) that key sets
  dp/src/dp/defaults_profile.gen.hpp  dp::defaultsProfile() -- every global that
                                    resetInvocationState (dp/src/dp/reset.hpp) sets to a
                                    constant, with its value; read right after that reset it is
                                    the core's built-in default profile

The lists are generated from those parsers rather than written by hand, so a key or a default
added there shows up here at the next generation -- and `--check` (run by
py/test_effective_cfg.py) fails while the headers are behind the code.

A parser branch whose target cannot be read off mechanically must be listed in UNRESOLVED with
the reason; an unknown one stops the generation.
"""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SESSION = ROOT / "src" / "mod" / "session.hpp"
RESET = ROOT / "dp" / "src" / "dp" / "reset.hpp"
OUT_MOD = ROOT / "src" / "mod" / "cfg_effective.gen.hpp"
OUT_DP = ROOT / "dp" / "src" / "dp" / "defaults_profile.gen.hpp"
PARSERS = ("loadConfig", "loadDpCfg", "loadTraceCfg")

# Keys whose branch sets nothing that could be printed, and why.
UNRESOLVED = {
    "dpspentorb": "accepted and ignored (always on)",
    "regionbox": "four floats into itermap::g_regionBox (print only, cfg regionat)",
    "batchlog": "two ticks into itermap::g_batchFrom / g_batchTo (print only)",
    "objwatch": "a uid list into itermap::g_objWatch (print only)",
    "planfile": "a plan file (plan_io.hpp loadInputsFile): its last block into g_cfg.inputs, the earlier blocks into g_prelude",
}
# Keys that share one branch with others (`key == "a" || key == "b"`, the parser is at MSVC's
# nesting ceiling) or set their variable through a helper: which variable each one sets. Every
# name here must appear in the text right after that key's test, or the generation stops -- so a
# rename in the parser cannot leave this table pointing at the old variable.
SHARED = {
    "touchpayload": ["g_cfg.touchPayload"],
    "portalpayload": ["g_cfg.portalPayload"],
    "capture": ["g_cfg.researchCapture"],
    "histride": ["g_cfg.histRide"],
    "anchorframe2dir": ["g_cfg.anchorFrame2Dir"],
    "hitboxtrace": ["g_cfg.hitboxTrace"],
    "fieldprobe": ["g_cfg.fieldProbe"],
    "killersite": ["g_cfg.killerSite"],
    "nodeath": ["g_cfg.noDeathCode"],
    "dppin": ["g_cfg.dpPin"],
    "dptap": ["g_cfg.dpTap"],
    "dpswitch": ["g_cfg.dpSwitch"],
    "dpincanchor": ["g_cfg.dpIncAnchor"],
    "dprlbound": ["g_cfg.dpRlBound"],
    "dprlspread0": ["g_cfg.dpRlSpread0"],
    "dpstatecross": ["g_cfg.dpStateCross"],
    "dpareaadd": ["g_cfg.dpAreaAdd"],
    "dpcrosscx": ["g_cfg.dpCrossCx"],
    "dpcrossbehind": ["g_cfg.dpCrossBehind"],
    "dpwaveflipcap": ["g_cfg.dpWaveFlipCap"],
    "dprampseatall": ["g_cfg.dpRampSeatAll"],
    "dpslopelagplain": ["g_cfg.dpSlopeLagPlain"],
    "dpwaveflipcapfloor": ["g_cfg.dpWaveFlipCapFloor"],
    "dpwaveflipceilup": ["g_cfg.dpWaveFlipCeilUp"],
    "dpwaveportalrotmini": ["g_cfg.dpWavePortalRotMini"],
    "dpballseatground": ["g_cfg.dpBallSeatGround"],
    "dprobotslopenohover": ["g_cfg.dpRobotSlopeNoHover"],
    "dpridesupgrace": ["g_cfg.dpRideSupGrace"],
    "dpportalobbmode": ["g_cfg.dpPortalObbMode"],
    "dpcrossminlag": ["g_cfg.dpCrossMinLag"],
    "dpdualbornlinked": ["g_cfg.dpDualBornLinked"],
    "dpbirthrung": ["g_cfg.dpBirthRung"],
    "hbfrom": ["g_cfg.hbFrom"],
    "coins": ["g_cfg.coinMode"],
    "coinroute": ["g_cfg.coinRoute"],
    "coinwatch": ["g_cfg.coinWatch"],
    "worldgates": ["g_cfg.worldGates"],
    "watchspeed": ["g_watchIdx"],
}
SHARED_REACH = 2200   # characters after the key's test in which its variable must be named

GLOBAL = r"(?:\w+::)*g_\w+(?:\.\w+)*"
DECL = re.compile(r"\b(?:auto|bool|int|long long|long|double|float|size_t|unsigned|"
                  r"std::string|const\s+[\w:<>]+)\s+(\w+\s*(?:=|;|\()[^;]*);")


def local_names(branch: str) -> set[str]:
    """Every name a declaration in the branch introduces, `long long a = 0, b = -1;` included."""
    names: set[str] = set()
    for m in DECL.finditer(branch):
        for part in m.group(1).split(","):
            n = re.match(r"\s*(\w+)", part)
            if n:
                names.add(n.group(1))
    return names

FMT = r'''
template <class T> struct IsAtomic : std::false_type {};
template <class T> struct IsAtomic<std::atomic<T>> : std::true_type {};
template <class T> struct IsVector : std::false_type {};
template <class T, class A> struct IsVector<std::vector<T, A>> : std::true_type {};

template <class T> inline void one(std::string& s, const T& v) {
    if constexpr (IsAtomic<T>::value) {
        one(s, v.load());
    } else if constexpr (std::is_same_v<T, bool>) {
        s += v ? "1" : "0";
    } else if constexpr (std::is_enum_v<T>) {
        s += std::to_string((long long)v);
    } else if constexpr (std::is_integral_v<T>) {
        s += std::to_string(v);
    } else if constexpr (std::is_floating_point_v<T>) {
        char b[40];
        std::snprintf(b, sizeof(b), "%.9g", (double)v);
        s += b;
    } else if constexpr (std::is_pointer_v<T>) {
        // before the string case: a null `const char*` would be read as a string and crash
        if (!v) {
            s += "null";
        } else if constexpr (std::is_same_v<std::remove_cv_t<std::remove_pointer_t<T>>, char>) {
            one(s, std::string(v));
        } else {
            s += "set";
        }
    } else if constexpr (std::is_convertible_v<T, std::string>) {
        const std::string t(v);
        if (t.empty()) s += "-";
        for (char c : t) s += (c == ' ') ? '_' : c;
    } else if constexpr (IsVector<T>::value) {
        if (v.empty()) s += "-";
        for (size_t i = 0; i < v.size(); ++i) {
            if (i) s += ';';
            one(s, v[i]);
        }
    } else {
        s += "?";
    }
}

template <class... T> inline void put(std::string& s, const char* key, const T&... v) {
    if (!s.empty()) s += ' ';
    s += key;
    s += '=';
    int i = 0;
    auto item = [&](const auto& x) {
        if (i++) s += ',';
        one(s, x);
    };
    (item(v), ...);
}
'''


def body_of(text: str, name: str) -> str:
    m = re.search(r"^inline\s+\S+\s+" + name + r"\s*\(", text, re.M)
    if not m:
        raise SystemExit(f"{name}: not found")
    end = re.search(r"^}", text[m.end():], re.M)
    return text[m.end(): m.end() + end.start()]


def strip_comments(s: str) -> str:
    s = re.sub(r"/\*.*?\*/", "", s, flags=re.S)
    return re.sub(r"//[^\n]*", "", s)


def mod_keys(text: str) -> list[tuple[str, list[str]]]:
    out: list[tuple[str, list[str]]] = []
    seen: set[str] = set()
    unknown: list[str] = []
    for fn in PARSERS:
        body = strip_comments(body_of(text, fn))
        hits = list(re.finditer(r'key\s*==\s*"([^"]+)"', body))
        for i, h in enumerate(hits):
            key = h.group(1)
            branch = body[h.end(): hits[i + 1].start() if i + 1 < len(hits) else len(body)]
            if key in seen:
                continue
            seen.add(key)
            if key in SHARED:
                near = body[h.end(): h.end() + SHARED_REACH]
                missing = [t for t in SHARED[key] if t not in near]
                if missing:
                    raise SystemExit(f"SHARED[{key!r}] names {missing}, which the parser no "
                                     f"longer sets right after that key's test")
                out.append((key, SHARED[key]))
                continue
            local = local_names(branch)
            targets: list[str] = []
            for m in re.finditer(r"cfgNum\(\s*[^,]+,\s*[^,]+,\s*(" + GLOBAL + r")\s*\)", branch):
                targets.append(m.group(1))
            # an assignment from the value, or from a local the branch parsed out of it
            for m in re.finditer(r"(" + GLOBAL + r")\s*=(?!=)\s*([^;]*);", branch):
                rhs = m.group(2)
                if re.search(r"\bval\b", rhs) or any(re.search(r"\b" + re.escape(n) + r"\b", rhs)
                                                     for n in local):
                    targets.append(m.group(1))
            for m in re.finditer(r"(" + GLOBAL + r")\.(?:push_back|clear)\(", branch):
                targets.append(m.group(1))
            targets = list(dict.fromkeys(t for t in targets if t.split(".")[0].split("::")[-1] not in local))
            if targets:
                out.append((key, targets))
            elif key not in UNRESOLVED:
                unknown.append(f"{fn}: {key}")
    if unknown:
        raise SystemExit("no target could be read for:\n  " + "\n  ".join(unknown)
                         + "\nlist each in UNRESOLVED with the reason, or give it a plain target")
    return out


CONST = re.compile(r"^(?:-?\d[\d.']*(?:[eE][-+]?\d+)?[fFuUlL]*|true|false|nullptr|\"[^\"]*\"|"
                   r"-?(?:[A-Z]\w*::)+\w+|-?k[A-Z]\w*|-?(?:\d[\d.]*)\s*[*/+-]\s*(?:\d[\d.]*))$")


def dp_defaults(text: str) -> list[str]:
    body = strip_comments(body_of(text, "resetInvocationState"))
    out: list[str] = []
    for m in re.finditer(r"^\s*(g_\w+(?:\.\w+)?)\s*=\s*([^;]+);", body, re.M):
        if CONST.match(m.group(2).strip()):
            out.append(m.group(1))
    return list(dict.fromkeys(out))


def gen_mod(keys: list[tuple[str, list[str]]]) -> str:
    lines = ["// GENERATED by tools/gen_effective_cfg.py from src/mod/session.hpp -- do not edit.",
             "// Every autorun.cfg key the mod's parser knows, with the value of what it sets.",
             "#pragma once",
             "#include <atomic>", "#include <cstdio>", "#include <string>",
             "#include <type_traits>", "#include <vector>", "",
             "namespace p1 {", "namespace effcfg {", FMT.strip("\n"), "",
             "inline std::string modCfg() {", "    std::string s;"]
    for key, targets in keys:
        lines.append(f'    put(s, "{key}", {", ".join(targets)});')
    lines += ["    return s;", "}", "", "}  // namespace effcfg", "}  // namespace p1", ""]
    return "\n".join(lines)


def gen_dp(names: list[str]) -> str:
    lines = ["// GENERATED by tools/gen_effective_cfg.py from dp/src/dp/reset.hpp -- do not edit.",
             "// Every global resetInvocationState sets to a constant, with its current value: read",
             "// right after that reset, the core's built-in default profile.",
             "#pragma once",
             "#include <atomic>", "#include <cstdio>", "#include <string>",
             "#include <type_traits>", "#include <vector>", "",
             "namespace dp {", "namespace effdef {", FMT.strip("\n"), "}  // namespace effdef", "",
             "inline std::string defaultsProfile() {", "    using effdef::put;", "    std::string s;"]
    for n in names:
        lines.append(f'    put(s, "{n}", {n});')
    lines += ["    return s;", "}", "", "}  // namespace dp", ""]
    return "\n".join(lines)


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--check", action="store_true")
    a = ap.parse_args()
    mod = gen_mod(mod_keys(SESSION.read_text(encoding="utf-8")))
    dp = gen_dp(dp_defaults(RESET.read_text(encoding="utf-8")))
    stale = []
    for path, text in ((OUT_MOD, mod), (OUT_DP, dp)):
        old = path.read_text(encoding="utf-8") if path.exists() else None
        if old != text:
            stale.append(path.relative_to(ROOT).as_posix())
            if not a.check:
                path.write_text(text, encoding="utf-8", newline="\n")
    if a.check:
        if stale:
            print("stale (run tools/gen_effective_cfg.py): " + ", ".join(stale))
            return 1
        print("up to date")
        return 0
    print("wrote " + (", ".join(stale) if stale else "nothing (up to date)"))
    return 0


if __name__ == "__main__":
    sys.exit(main())
