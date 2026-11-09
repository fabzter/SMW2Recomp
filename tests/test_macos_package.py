#!/usr/bin/env python3
"""Regression: the macOS bundle payload must exclude per-machine configs and
test/build artifacts that land at the build tree's top level.

Runs the real scripts/macos/package-app.sh against a synthetic build dir so
no real install or existing bundle is touched.
"""
import os
import stat
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
SCRIPT = ROOT / "scripts" / "macos" / "package-app.sh"
EXE = "SuperMarioWorld2YoshisIslandSNESRecomp"

PAYLOAD_DIRS = ("assets", "launcher_assets", "mods")
PAYLOAD_FILES = (EXE, "shadercache.txt")


def make_exe(path: Path) -> None:
    path.write_text("#!/bin/sh\nexit 0\n")
    path.chmod(path.stat().st_mode | stat.S_IXUSR | stat.S_IXGRP | stat.S_IXOTH)


def main() -> int:
    with tempfile.TemporaryDirectory() as td:
        build = Path(td) / "build"
        out = Path(td) / "dist"
        build.mkdir()

        for d in PAYLOAD_DIRS:
            (build / d / "x").mkdir(parents=True)
            (build / d / "x" / "keep").write_text("1")
        make_exe(build / EXE)
        for f in PAYLOAD_FILES:
            if f != EXE:
                (build / f).write_text("keep")

        # Leaks: per-machine config variants, test binary, CTest artifacts.
        for leak in (
            "config.local.ini",
            "config.dbg.ini",
            "config.ini",        # already excluded — pin it
            "smw2_ws_test",
            "smw2_ws_geometry",
            "smw2_bg_render",
            "smw2_state_load",
            "smw2_host_wiring",
            "smw2_runahead",
            "CTestTestfile.cmake",
            "DartConfiguration.tcl",
            "Testing",
            f"{EXE}.dSYM",
        ):
            p = build / leak
            if leak in ("Testing", f"{EXE}.dSYM"):
                (p / "x").mkdir(parents=True)
                (p / "x" / "leak").write_text("1")
            else:
                (p).write_text("leak")

        subprocess.run(
            ["bash", str(SCRIPT), str(build), str(out)],
            cwd=ROOT, check=True, capture_output=True, text=True,
        )

        macos = out / "Yoshi's Island Recomp.app" / "Contents" / "MacOS"
        missing, leaked = [], []
        for name in PAYLOAD_DIRS + PAYLOAD_FILES:
            if not (macos / name).exists():
                missing.append(name)
        for name in (
            "config.local.ini", "config.dbg.ini", "config.ini",
            "smw2_ws_test",
            "smw2_ws_geometry",
            "smw2_bg_render",
            "smw2_state_load",
            "smw2_host_wiring",
            "smw2_runahead", "CTestTestfile.cmake", "DartConfiguration.tcl",
            "Testing", f"{EXE}.dSYM",
        ):
            if (macos / name).exists():
                leaked.append(name)
        # symlinked launcher roots still resolve
        res = out / "Yoshi's Island Recomp.app" / "Contents" / "Resources"
        for d in ("assets", "launcher_assets", "mods"):
            if not (res / d).is_symlink():
                missing.append(f"Resources/{d} symlink")

        for name in missing:
            print(f"MISSING payload: {name}", file=sys.stderr)
        for name in leaked:
            print(f"LEAKED into bundle: {name}", file=sys.stderr)
        if missing or leaked:
            return 1
        print("package payload clean")
        return 0


if __name__ == "__main__":
    sys.exit(main())
