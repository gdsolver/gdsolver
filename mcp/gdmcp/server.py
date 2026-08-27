"""The MCP server (stdio).

    python -m gdmcp.server

The tools themselves live in tools.py. This file holds only the registration and the
guarantee that workers are cleaned up when the process exits (leaving a GD held open
means the next open is blocked by our own leftovers).
"""

from __future__ import annotations

import atexit
import sys

from mcp.server.fastmcp import FastMCP

from . import tools

mcp = FastMCP("gd")

for fn in tools.TOOLS:
    mcp.tool()(fn)


@atexit.register
def _cleanup() -> None:
    try:
        tools.gd_session_close()
    except Exception:
        pass


def main() -> None:
    try:
        mcp.run()
    finally:
        _cleanup()


if __name__ == "__main__":
    sys.exit(main())
