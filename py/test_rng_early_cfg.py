"""cfg rngearly is ONE variable that the effective cfg line prints and snapshotGlobals saves (AUD-866).

The setting used to be a bool and a seed; the generated cfg_effective.gen.hpp listed only the seed, so
the effective line could not tell off from on and a level that turned it on left it on for the next.
Static checks (no game): the parser branch assigns exactly one variable, that variable is printed
(put) and saved (snapOne) in the generated header, and the old bool is gone from src/mod.
The parse itself and the default -> on -> restore sequence are in tools/test_rng_early.cpp.
"""

from __future__ import annotations

import re
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
MOD = ROOT / "src" / "mod"


def branch(text: str, key: str) -> str:
    i = text.index('if (key == "%s")' % key)
    j = text.index("return true;", i)
    return text[i:j]


class RngEarlyIsOneVariable(unittest.TestCase):
    def test_parser_assigns_one_variable(self):
        b = branch((MOD / "session.hpp").read_text(encoding="utf-8"), "rngearly")
        targets = set(re.findall(r"\b(g_\w+)\s*=[^=]", b))
        self.assertEqual(targets, {"g_rngEarlySeed"}, targets)

    def test_effective_line_and_snapshot_name_it(self):
        gen = (MOD / "cfg_effective.gen.hpp").read_text(encoding="utf-8")
        self.assertRegex(gen, r'put\(s, "rngearly", g_rngEarlySeed\);')
        self.assertIn("snapOne(g_rngEarlySeed);", gen)

    def test_the_old_bool_is_gone(self):
        for p in MOD.glob("*"):
            if p.suffix in (".hpp", ".cpp"):
                self.assertIsNone(re.search(r"\bg_rngEarly\b", p.read_text(encoding="utf-8", errors="replace")), p.name)

    def test_default_is_on_with_the_builtin_seed(self):
        c = (MOD / "config.hpp").read_text(encoding="utf-8")
        self.assertRegex(c, r"inline long long g_rngEarlySeed = kRngEarlySeed;")
        r = (MOD / "rng_early.hpp").read_text(encoding="utf-8")
        self.assertRegex(r, r"constexpr long long kRngEarlySeed = 0x48c67641LL;")


if __name__ == "__main__":
    unittest.main()
