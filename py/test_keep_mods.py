"""A worker's keep_mods.txt: the packages _prepare leaves in geode/mods. (no GD; seconds)

    python py/test_keep_mods.py
"""
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

from gdtas.worker import KEEP_MODS_FILE, read_keep_mods  # noqa: E402


class KeepMods(unittest.TestCase):
    def read(self, text):
        with tempfile.TemporaryDirectory() as d:
            if text is not None:
                (Path(d) / KEEP_MODS_FILE).write_text(text, encoding="utf-8")
            return read_keep_mods(Path(d))

    def test_no_file_keeps_nothing(self):
        self.assertEqual(self.read(None), (set(), []))

    def test_names_blank_lines_and_comments(self):
        keep, refused = self.read("# kept\n\na.b.geode\n  c.geode  \n")
        self.assertEqual(keep, {"a.b.geode", "c.geode"})
        self.assertEqual(refused, [])

    def test_only_bare_geode_names(self):
        bad = ["..\\x.geode", "../x.geode", "sub/x.geode", "sub\\x.geode", "C:x.geode", "x.dll", "x", ".."]
        keep, refused = self.read("ok.geode\n" + "\n".join(bad) + "\n")
        self.assertEqual(keep, {"ok.geode"})
        self.assertEqual(refused, bad)

    def test_bom_is_tolerated(self):
        with tempfile.TemporaryDirectory() as d:
            (Path(d) / KEEP_MODS_FILE).write_bytes(b"\xef\xbb\xbfa.geode\n")
            self.assertEqual(read_keep_mods(Path(d)), ({"a.geode"}, []))


if __name__ == "__main__":
    unittest.main()