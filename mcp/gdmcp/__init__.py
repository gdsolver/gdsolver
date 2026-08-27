"""gdmcp -- an MCP server that drives GD for analysis (phase 1).

Phase 1 leaves THE MOD UNMODIFIED. It uses the existing servemode / cmd.txt /
plan_in.txt / dump.csv / trace.csv as they are. It never touches C++, so it does
not conflict with the main line's build.
"""

__all__ = ["worker", "data", "tools"]
