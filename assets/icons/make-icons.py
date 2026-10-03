#!/usr/bin/env python3
"""Regenerates every icon asset in this directory from the two masters.

Committed outputs, committed generator. The alternative -- generating at build
time -- would put ImageMagick on the dependency list of every build machine and
every CI runner, for files that change when the artwork changes and never
otherwise.

One master, xpcog.png: the free-form gear, transparent. Windows app icons and
Linux ones are free-form, and a tray icon has to read at 16px against a
background whose colour is not ours to know. (xpcog-tile.png, the same artwork
on a rounded-square backdrop, was the macOS bundle's master; it is kept as
artwork, and nothing is generated from it since 2.0.0.)

Run from anywhere:  python assets/icons/make-icons.py
Needs ImageMagick 7 on PATH, or set MAGICK to point at magick.exe.
"""

from __future__ import annotations

import os
import shutil
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent

# The sizes the Qt resource carries. Given to QIcon individually rather than
# left to it to scale one large PNG: Qt's smooth downscale of 256px artwork with
# this much fine detail in the gear teeth is visibly muddier at 16px than a
# dedicated resize, and 16px is the size the tray actually uses.
PNG_SIZES = (16, 24, 32, 48, 64, 128, 256)

# Windows wants these in one .ico. 20 and 40 are the awkward ones -- they are
# what 16 and 32 become at 125% display scaling, and without them Windows scales
# the 32 down to 20 itself, badly.
ICO_SIZES = (256, 128, 64, 48, 40, 32, 24, 20, 16)

def magick() -> str:
    override = os.environ.get("MAGICK")
    if override:
        return override
    found = shutil.which("magick")
    if not found:
        sys.exit("ImageMagick 7 not found: put `magick` on PATH or set MAGICK.")
    return found


def resize(tool: str, source: Path, size: int, destination: Path) -> None:
    subprocess.run(
        # -depth 8: an HDRI/Q16 build otherwise writes 16-bit PNGs, at three
        # times the size for no visible difference.
        [tool, str(source), "-resize", f"{size}x{size}", "-strip", "-depth", "8", str(destination)],
        check=True,
    )


def main() -> None:
    tool = magick()
    free_form = HERE / "xpcog.png"
    if not free_form.exists():
        sys.exit(f"missing master: {free_form}")

    print("PNG set (free-form, for the Qt resource)")
    for size in PNG_SIZES:
        resize(tool, free_form, size, HERE / f"xpcog-{size}.png")

    print("xpcog.ico (free-form, for the Windows executable)")
    subprocess.run(
        [
            tool,
            str(free_form),
            "-define",
            "icon:auto-resize=" + ",".join(str(size) for size in ICO_SIZES),
            str(HERE / "xpcog.ico"),
        ],
        check=True,
    )


if __name__ == "__main__":
    main()
