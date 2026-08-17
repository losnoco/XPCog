#!/usr/bin/env python3
"""Regenerates every icon asset in this directory from the two masters.

Committed outputs, committed generator. The alternative -- generating at build
time -- would put ImageMagick on the dependency list of every build machine and
every CI runner, for files that change when the artwork changes and never
otherwise.

Two masters, because the platforms want different shapes:

  xpcog.png       the free-form gear, transparent. Windows app icons and Linux
                  ones are free-form, and a tray icon has to read at 16px
                  against a background whose colour is not ours to know -- the
                  tile's near-white backdrop would show up as a pale box in a
                  dark notification area.
  xpcog-tile.png  the same artwork on a rounded-square backdrop. This is the
                  macOS convention: a Dock icon is expected to be a shape, and
                  Cog's own icon is a tile.

Run from anywhere:  python app/icons/make-icons.py
Needs ImageMagick 7 on PATH, or set MAGICK to point at magick.exe.
"""

from __future__ import annotations

import os
import shutil
import struct
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

# The .icns chunk types, as `iconutil` writes them from a complete .iconset.
# Sizes repeat because macOS asks for the same pixel count under two names: ic11
# is 16pt at 2x and icp5 is 32pt at 1x, and both are 32 pixels square.
ICNS_CHUNKS = (
    (b"icp4", 16),
    (b"icp5", 32),
    (b"ic11", 32),
    (b"ic12", 64),
    (b"ic07", 128),
    (b"ic13", 256),
    (b"ic08", 256),
    (b"ic14", 512),
    (b"ic09", 512),
    (b"ic10", 1024),
)


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
        [tool, str(source), "-resize", f"{size}x{size}", "-strip", str(destination)],
        check=True,
    )


def write_icns(pngs: dict[int, bytes], destination: Path) -> None:
    """Packs PNGs into an .icns container.

    Written here because ImageMagick's ICNS coder is read-only in the standard
    Windows build (`IM_MOD_RL_ICNS_.dll` is simply absent), and `iconutil` is
    macOS-only -- so on this host neither tool can produce the file the macOS
    bundle needs. The format is small enough to emit directly: a magic, a total
    length, then typed chunks whose payload for every modern icon type is a
    plain PNG.

    Big-endian throughout, and each chunk's length *includes* its own 8-byte
    header. Getting that wrong produces a file macOS silently declines to draw
    rather than one it reports as corrupt, which is why the reader below
    verifies it.
    """
    body = bytearray()
    for chunk_type, size in ICNS_CHUNKS:
        payload = pngs[size]
        body += chunk_type
        body += struct.pack(">I", len(payload) + 8)
        body += payload

    destination.write_bytes(b"icns" + struct.pack(">I", len(body) + 8) + bytes(body))


def verify_icns(path: Path) -> None:
    """Parses the file back, so a packing slip fails here and not on a Mac."""
    data = path.read_bytes()
    if data[:4] != b"icns":
        sys.exit("icns: bad magic")
    total = struct.unpack(">I", data[4:8])[0]
    if total != len(data):
        sys.exit(f"icns: header says {total} bytes, file is {len(data)}")

    offset, seen = 8, []
    while offset < len(data):
        chunk_type = data[offset : offset + 4]
        length = struct.unpack(">I", data[offset + 4 : offset + 8])[0]
        if length < 8 or offset + length > len(data):
            sys.exit(f"icns: chunk {chunk_type!r} has impossible length {length}")
        if data[offset + 8 : offset + 16] != b"\x89PNG\r\n\x1a\n":
            sys.exit(f"icns: chunk {chunk_type!r} payload is not a PNG")
        seen.append(chunk_type.decode())
        offset += length

    expected = [chunk.decode() for chunk, _ in ICNS_CHUNKS]
    if seen != expected:
        sys.exit(f"icns: chunks {seen} != {expected}")
    print(f"  verified {path.name}: {len(seen)} chunks, {len(data)} bytes")


def main() -> None:
    tool = magick()
    free_form = HERE / "xpcog.png"
    tile = HERE / "xpcog-tile.png"
    for master in (free_form, tile):
        if not master.exists():
            sys.exit(f"missing master: {master}")

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

    print("xpcog.icns (tile, for the macOS bundle)")
    scratch = HERE / ".icns-tmp"
    scratch.mkdir(exist_ok=True)
    try:
        pngs: dict[int, bytes] = {}
        for size in sorted({size for _, size in ICNS_CHUNKS}):
            staged = scratch / f"{size}.png"
            resize(tool, tile, size, staged)
            pngs[size] = staged.read_bytes()
        write_icns(pngs, HERE / "xpcog.icns")
    finally:
        shutil.rmtree(scratch, ignore_errors=True)
    verify_icns(HERE / "xpcog.icns")


if __name__ == "__main__":
    main()
