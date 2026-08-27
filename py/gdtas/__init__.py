"""Shared library for the GD TAS tool layer (the migration target from PowerShell).

Worker lifecycle management, the serve protocol and the result.txt contract live
HERE AND NOWHERE ELSE. Implement them twice and a fix for a trap hit on one side
never reaches the other.
"""

__all__ = ["paths", "results", "plan", "worker", "provision", "memory",
           "gdsave", "window", "observe"]
