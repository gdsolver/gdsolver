"""Plan masks without a game: the readers keep or refuse player 2's button, and a launch of a plan that carries it is
refused for a package that does not press it.
    python py/test_plan_masks.py
`input=<tick>,<mask>`: bit 0 is player 1's jump, bit 1 player 2's. A reader that squashed the mask to one button would hand the
mod a plan with player 2's presses gone (and a 2 as a press of player 1); an older package reads a cfg `input=T,2` as a release."""
from __future__ import annotations

import sys
import tempfile
import unittest
import zipfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

from gdtas import gdbase, plan  # noqa: E402
from gdtas.worker import WorkerError, check_plan_buttons, plan_masks  # noqa: E402


def package(path: Path, dll_bytes: bytes) -> Path:
    with zipfile.ZipFile(path, "w") as z:
        z.writestr("mod.json", "{}")
        z.writestr("gdsolver.solver.dll", dll_bytes)
    return path


class PlanMasks(unittest.TestCase):
    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp())

    def write(self, name: str, body: str) -> Path:
        p = self.tmp / name
        p.write_text(body, encoding="utf-8")
        return p

    def test_one_button_plan_reads_as_before(self):
        p = self.write("a.txt", "input=10,1\ninput=12,0\n")
        self.assertEqual(plan.read_inputs(p), [(10, 1), (12, 0)])
        self.assertEqual(plan.read_input_lines(p), ["input=10,1", "input=12,0"])

    def test_masks_keep_both_buttons(self):
        p = self.write("b.txt", "input=10,2\ninput=12,3\ninput=14,0\n")
        self.assertEqual(plan.read_input_masks(p), [(10, 2), (12, 3), (14, 0)])
        # the lines handed to the mod are the file's, not a squashed form
        self.assertEqual(plan.read_input_lines(p), ["input=10,2", "input=12,3", "input=14,0"])

    def test_one_button_reader_refuses_player_2(self):
        p = self.write("c.txt", "input=10,1\ninput=12,2\n")
        with self.assertRaises(ValueError):
            plan.read_inputs(p)

    def test_buttons_nobody_drives_are_refused(self):
        p = self.write("d.txt", "input=10,4\n")
        with self.assertRaises(ValueError):
            plan.read_input_masks(p)

    def test_plan_masks_of_cfg_lines(self):
        self.assertEqual(plan_masks(["enabled=1", "input=1,1", "input=5,2", " input=9,3", "input=x"]), [1, 2, 3])

    def test_launch_guard(self):
        old = package(self.tmp / "old.geode", b"MZ nothing about the second player")
        new = package(self.tmp / "new.geode", b"MZ plan: " + gdbase.PLAYER2_INPUT_MARK + b" in a level")
        one_button = ["input=1,1", "input=3,0"]
        with_p2 = ["input=1,1", "input=3,2"]
        check_plan_buttons(one_button, old)       # a plan of one button launches on either package
        check_plan_buttons(one_button, new)
        check_plan_buttons(with_p2, new)
        with self.assertRaises(WorkerError):
            check_plan_buttons(with_p2, old)      # ...player 2's button only on one that presses it
        with self.assertRaises(WorkerError):
            check_plan_buttons(["input=1,4"], new)
        with self.assertRaises(WorkerError):
            check_plan_buttons(with_p2, self.tmp / "missing.geode")   # a package that cannot be read is not one that presses it


if __name__ == "__main__":
    unittest.main()
