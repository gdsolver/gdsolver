"""Tests for py/gdref_addcols.py: the columns come over only from the same run, and the reference's own cells are written back as they were."""
from __future__ import annotations

import csv
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import gdref_addcols as G

REF_HDR = ["attempt", "tick", "x", "y", "yvel", "mode", "dual", "upsideDown", "rot"]
SRC_HDR = ["attempt", "tick", "x", "y", "yvel", "mode", "dual", "upsideDown", "rot", "boost", "freemode"]


def write(path: Path, hdr: list[str], rows: list[list[str]]) -> None:
    with path.open("w", newline="", encoding="utf-8") as f:
        w = csv.writer(f, lineterminator="\n")
        w.writerow(hdr)
        w.writerows(rows)


def ref_rows() -> list[list[str]]:
    return [["1", str(t), "%.6f" % (t * 1.5), "105", "0", "cube", "0", "0", "0"] for t in range(1, 6)]


def src_rows(free_from: int = 4) -> list[list[str]]:
    # the same run; the dump prints y and x with more digits, and rot differs (a column the tool does not compare)
    return [["1", str(t), "%.9f" % (t * 1.5), "105.0000001", "0.0", "cube", "0", "0", "17", "0", "1" if t >= free_from else "0"] for t in range(1, 6)]


class AddCols(unittest.TestCase):
    def setUp(self):
        self.d = Path(tempfile.mkdtemp())
        self.ref, self.src, self.out = self.d / "ref", self.d / "src", self.d / "out"
        self.ref.mkdir(); self.src.mkdir()

    def run_tool(self, *extra: str) -> int:
        return G.main(["--ref", str(self.ref), "--src", str(self.src), "--out", str(self.out), "--levels", "1", *extra])

    def test_same_run_gets_the_column_and_keeps_its_own_strings(self):
        write(self.ref / "lv1.csv", REF_HDR, ref_rows())
        write(self.src / "lv1.csv", SRC_HDR, src_rows())
        self.assertEqual(self.run_tool(), 0)
        rows = list(csv.reader((self.out / "lv1.csv").open(encoding="utf-8")))
        self.assertEqual(rows[0], REF_HDR + ["freemode"])
        self.assertEqual([r[-1] for r in rows[1:]], ["0", "0", "0", "1", "1"])
        self.assertEqual([r[:len(REF_HDR)] for r in rows[1:]], ref_rows())   # the reference's cells, character for character

    def test_the_reference_is_not_overwritten(self):
        write(self.ref / "lv1.csv", REF_HDR, ref_rows())
        write(self.src / "lv1.csv", SRC_HDR, src_rows())
        before = (self.ref / "lv1.csv").read_bytes()
        self.assertEqual(G.main(["--ref", str(self.ref), "--src", str(self.src), "--out", str(self.ref), "--levels", "1"]), 2)
        self.assertEqual((self.ref / "lv1.csv").read_bytes(), before)

    def test_a_different_run_is_refused(self):
        write(self.ref / "lv1.csv", REF_HDR, ref_rows())
        bad = src_rows()
        bad[2][2] = "99.000000000"   # x differs on row 3
        write(self.src / "lv1.csv", SRC_HDR, bad)
        self.assertEqual(self.run_tool(), 1)
        self.assertFalse((self.out / "lv1.csv").exists())

    def test_y_beyond_the_tolerance_is_refused_and_inside_it_is_not(self):
        write(self.ref / "lv1.csv", REF_HDR, ref_rows())
        far = src_rows()
        far[0][3] = "105.01"
        write(self.src / "lv1.csv", SRC_HDR, far)
        self.assertEqual(self.run_tool("--tol-y", "1e-3"), 1)
        self.assertEqual(self.run_tool("--tol-y", "0.1"), 0)

    def test_a_missing_row_is_refused(self):
        write(self.ref / "lv1.csv", REF_HDR, ref_rows())
        write(self.src / "lv1.csv", SRC_HDR, src_rows()[:-1])
        self.assertEqual(self.run_tool(), 1)

    def test_a_raw_dump_is_a_source_too(self):
        write(self.ref / "lv1.csv", REF_HDR, ref_rows())
        write(self.src / "lv1.dump.csv", SRC_HDR, src_rows())
        self.assertEqual(self.run_tool(), 0)

    def test_a_column_the_reference_has_is_not_added_twice(self):
        write(self.ref / "lv1.csv", REF_HDR + ["freemode"], [r + ["0"] for r in ref_rows()])
        write(self.src / "lv1.csv", SRC_HDR, src_rows())
        self.assertEqual(self.run_tool(), 0)
        self.assertEqual(next(csv.reader((self.out / "lv1.csv").open(encoding="utf-8"))), REF_HDR + ["freemode"])


if __name__ == "__main__":
    unittest.main()
