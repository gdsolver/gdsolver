"""processes_under holds up while other threads scan the process table with other attributes.

The collector runs a thread per worker and every worker launch scans the processes; psutil.process_iter keeps one table of
Process objects for the whole interpreter and writes each caller's answer into `.info` on them, so a scan that asked for
"pid"/"exe" could read the dict another thread had just put there (KeyError 'pid', or no exe, a game left running unseen).
gdtas/procscan.py takes a fresh object per pid instead. (No game needed.)
"""

from __future__ import annotations

import os
import sys
import threading
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))

import psutil  # noqa: E402

from gdtas import procscan  # noqa: E402
from gdtas import worker as W  # noqa: E402


class PsutilSharedTable(unittest.TestCase):
    def test_process_iter_shares_info_between_threads(self):
        """The hazard itself, so a psutil that fixes it shows up here as a changed premise rather than a silent pass."""
        for _p in psutil.process_iter(["pid"]):      # an earlier scan has filled the shared table
            pass
        ev1, ev2 = threading.Event(), threading.Event()
        seen = {}
        me = os.getpid()

        def a():
            for p in psutil.process_iter(["pid", "exe"]):
                if p.pid == me:
                    ev1.set()
                    ev2.wait(10)
                    seen["info"] = dict(p.info)
                    return

        def b():
            ev1.wait(10)
            for _p in psutil.process_iter(["name"]):
                pass
            ev2.set()

        ta, tb = threading.Thread(target=a), threading.Thread(target=b)
        ta.start(); tb.start(); ta.join(); tb.join()
        self.assertNotIn("pid", seen["info"], "psutil no longer shares .info between threads: procscan may be simplified")


class ProcessesUnder(unittest.TestCase):
    def test_own_process_is_found_while_other_threads_scan(self):
        stop = threading.Event()

        def other():
            while not stop.is_set():
                for p in psutil.process_iter(["name"]):
                    p.info.get("name")

        threads = [threading.Thread(target=other, daemon=True) for _ in range(3)]
        for t in threads:
            t.start()
        try:
            root = Path(sys.executable).resolve().parent
            for _ in range(6):
                self.assertIn(os.getpid(), W.processes_under(root))
        finally:
            stop.set()

    def test_scan_gives_each_caller_its_own_attributes(self):
        pairs = procscan.scan(["pid", "name"])
        self.assertTrue(pairs)
        for p, info in pairs:
            self.assertEqual(set(info), {"pid", "name"})
            self.assertEqual(info["pid"], p.pid)


if __name__ == "__main__":
    unittest.main()
