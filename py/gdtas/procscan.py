"""A scan of the running processes that is safe to run from several threads at once.

`psutil.process_iter(attrs)` hands out Process objects it keeps in a table shared by the whole interpreter, and it writes
the answer into `proc.info` on those same objects. Two threads scanning with different attributes (one asks for "pid" and "exe",
another for "name") overwrite each other's `info` on the objects they both hold, and the first one then reads a dict that is
not the one it asked for: `KeyError: 'pid'` out of `processes_under`, or an `exe` that is simply missing, so a worker's
leftover game goes unseen. The collector runs one thread per worker and every worker launch scans, so it met this.

This takes a fresh Process object per pid, never from the shared table, and returns the answer next to it.
"""

from __future__ import annotations


def scan(attrs: list[str]):
    """[(Process, info dict)] for every process that is alive when it is looked at; `attrs` as in Process.as_dict.

    A value psutil may not read (AccessDenied) is None in `info`; a process that is gone is left out."""
    import psutil
    out = []
    for pid in psutil.pids():
        try:
            p = psutil.Process(pid)
            out.append((p, p.as_dict(attrs=attrs, ad_value=None)))
        except psutil.Error:
            pass
    return out
