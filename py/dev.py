"""Build, deploy to Geometry Dash and launch it, in one command.

    python py/dev.py                # build + deploy + launch GD
    python py/dev.py --no-run       # build + deploy only
    python py/dev.py --build-only   # compile check only (does not close GD)
    python py/dev.py --clean        # wipe build/ and rebuild from scratch

The Geometry Dash installation is found through the GD_DIR environment
variable, falling back to the default Steam library location; the Geode SDK
through GEODE_SDK (which the Geode CLI sets when it is installed).
"""

from __future__ import annotations

import argparse
import os
import re
import shutil
import subprocess
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

from gdtas.paths import REPO

def _gd_dir() -> Path:
    """Where Geometry Dash is installed.

    GD_DIR wins; otherwise ask the Geode CLI, which already knows (its default
    profile points at GeometryDash.exe); otherwise try the usual Steam library
    locations. A candidate only counts if the executable is really there --
    the previous hard-coded value survived a move of the install and silently
    pointed at a directory that no longer existed.
    """
    env = os.environ.get("GD_DIR")
    if env:
        return Path(env)
    try:
        out = subprocess.run(["geode", "profile", "list"], capture_output=True,
                             text=True, timeout=20).stdout
        m = re.search(r"path\s*=\s*(.+?GeometryDash\.exe)", out)
        if m:
            return Path(m.group(1).strip()).parent
    except (OSError, subprocess.SubprocessError):
        pass
    for p in (Path(r"C:\Program Files (x86)\Steam\steamapps\common\Geometry Dash"),
              *(Path(d) / "SteamLibrary" / "steamapps" / "common" / "Geometry Dash"
                for d in ("C:", "D:", "E:"))):
        if (p / "GeometryDash.exe").exists():
            return p
    raise SystemExit("Geometry Dash not found. Set GD_DIR to the install directory.")


GD_DIR = _gd_dir()
GEODE_SDK = Path(os.environ.get("GEODE_SDK")
                 or Path.home() / "Documents" / "Geode")


def _run(cmd: list[str], cwd: Path) -> int:
    print("+ " + " ".join(cmd))
    return subprocess.run(cmd, cwd=str(cwd)).returncode


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--no-run", action="store_true", help="build and deploy only")
    ap.add_argument("--clean", action="store_true",
                    help="delete build/ and do a full build")
    ap.add_argument("--build-only", action="store_true",
                    help="check that it compiles, nothing else: GD is neither "
                         "closed nor deployed to")
    a = ap.parse_args(argv)

    os.environ.setdefault("GEODE_SDK", str(GEODE_SDK))
    build = REPO / "build"

    if a.clean and build.exists():
        shutil.rmtree(build)

    if a.build_only:
        # cmake's post-build step still puts the .geode into the mods folder.
        # That succeeds without locking even while GD is running, but GD has to
        # be restarted to pick it up. What --build-only promises is only that
        # this script will not close GD itself.
        print("=== Build without closing GD ===")
        if _run(["cmake", "--build", str(build), "--config", "RelWithDebInfo"], REPO):
            print("BUILD FAILED")
            return 1
        print("BUILD OK - restart GD yourself to load it")
        return 0

    # A running GD holds the DLL locked, so close it first
    import psutil
    for p in psutil.process_iter(["pid", "name"]):
        if (p.info["name"] or "").lower() == "geometrydash.exe":
            print("GD is running - closing it before deploy...")
            try:
                p.kill()
                p.wait(10)
            except psutil.Error:
                pass

    print("=== Building gdsolver ===")
    if _run(["geode", "build"], REPO):
        print("BUILD FAILED")
        return 1

    # `geode build` deploys to the default profile (GD's mods folder) by itself.
    # Confirm the artefact is there anyway.
    pkgs = sorted(build.rglob("*.geode"))
    if pkgs:
        print(f"Built package: {pkgs[0]}")
    deployed = GD_DIR / "geode" / "mods" / "gdsolver.geode"
    if deployed.exists():
        print(f"Deployed: {deployed}")
    elif pkgs:
        deployed.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(pkgs[0], deployed)
        print(f"Copied package to: {deployed}")

    if not a.no_run:
        print("=== Launching GD (via Steam) ===")
        # IMPORTANT: launching GeometryDash.exe directly exits at once on the
        # Steam DRM check, and turns into a zombie when Geode is present. Always
        # go through the steam:// protocol.
        os.startfile("steam://rungameid/322170")   # noqa: S606
        print(f"Logs: {GD_DIR / 'geode' / 'logs'} (latest .log)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
