#!/usr/bin/env python3
"""Inspect a .UCE cartridge: check its layout and show what the game saved.

Usage:
    python3 tools/inspect_uce.py ALP-Snake.UCE              # check + summary
    python3 tools/inspect_uce.py ALP-Snake.UCE --list       # also list files
    python3 tools/inspect_uce.py ALP-Snake.UCE --extract out/   # copy save files

Why this exists: the cabinet keeps a game's saves INSIDE the .UCE file (the
4 MiB ext4 image at its end, mounted as an overlay on ./save). So the easiest
way to see what a game wrote on the real machine - high scores, settings, or
a probe build's log - is to copy the played .UCE back to a computer and look
inside it. This is how ALP Snake's probe log was read when the USB-copy
approach failed.

What it checks (the byte layout is described in tools/package_uce.py):
  * the file splits into squashfs + 64-byte trailer + 4 MiB save image;
  * the MD5 of the squashfs matches the one stored after it;
  * whether the save image's MD5 still matches. On a played cartridge it
    usually does NOT: the cabinet writes to the save image but does not
    update the stored MD5, and it loads the file anyway. So a mismatch is
    reported as information, not as an error.

Needs debugfs (Homebrew: `brew install e2fsprogs`) to read the save image,
and optionally unsquashfs (`brew install squashfs`) for --list.
"""

import argparse
import hashlib
import shutil
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

from package_uce import SAVE_SIZE, homebrew_tool

TRAILER = 64            # md5(squash) + 32 zero bytes + md5(save)
HIGH_SCORE_TABLE_MAGIC = 0x534E4B33     # "SNK3": current top-ten table
SETTINGS_MAGIC = 0x534E4B4F             # "SNKO": DIP-switch settings
SETTING_NAMES = ["hawk", "mouse", "slow signs", "move sound", "attract music"]


def find(name, formula):
    """Locate a command on PATH or inside its Homebrew formula."""
    return shutil.which(name) or homebrew_tool(formula, name)


def split_uce(data):
    """Split a UCE into (squashfs, stored squash md5, stored save md5, save)."""
    if len(data) < SAVE_SIZE + TRAILER + 4096:
        raise ValueError("file is too small to be a UCE")
    save = data[-SAVE_SIZE:]
    head = data[:-SAVE_SIZE]
    squash, trailer = head[:-TRAILER], head[-TRAILER:]
    return squash, trailer[:16], trailer[16:48], trailer[48:], save


def debugfs(image, command):
    """Run one read-only debugfs command against the save image."""
    tool = find("debugfs", "e2fsprogs")
    if not tool:
        raise RuntimeError("debugfs not found; install e2fsprogs")
    result = subprocess.run([tool, "-R", command, str(image)],
                            capture_output=True, check=False)
    return result.stdout


def describe_scores(data):
    """Decode save/hiscore.dat in ALP Snake's top-ten format."""
    if not data:
        return "  hiscore.dat: empty (the game shows its sample table)"
    if len(data) in (12, 16):
        # Older versions kept only the single best score: magic, score, the
        # score inverted (a cheap validity check) and, in "SNK2", initials.
        magic, best, _ = struct.unpack("<III", data[:12])
        name = data[12:15].decode(errors="replace") if len(data) == 16 else "---"
        return (f"  hiscore.dat (old single-best format {magic:#x}): {name} {best}"
                " - the game migrates it into the table")
    layout = "<II" + "I4s" * 10 + "I"
    if len(data) != struct.calcsize(layout):
        return f"  hiscore.dat: {len(data)} bytes, unknown format"
    fields = struct.unpack(layout, data)
    if fields[0] != HIGH_SCORE_TABLE_MAGIC:
        return "  hiscore.dat: unknown magic"
    lines = ["  hiscore.dat (top ten):"]
    for rank in range(10):
        score, name = fields[2 + rank * 2], fields[3 + rank * 2]
        if score:
            initials = name.rstrip(bytes(1)).decode(errors="replace")
            lines.append(f"    {rank + 1:2}. {initials:3} {score}")
    return "\n".join(lines)


def describe_settings(data):
    """Decode save/settings.dat: magic, bits, and the bits inverted."""
    if len(data) != 12:
        return f"  settings.dat: {len(data)} bytes (not present or unknown)"
    magic, bits, inverse = struct.unpack("<III", data)
    if magic != SETTINGS_MAGIC or bits != (~inverse & 0xFFFFFFFF):
        return "  settings.dat: invalid (the game would use its defaults)"
    states = ", ".join(f"{name} {'on' if bits >> i & 1 else 'off'}"
                       for i, name in enumerate(SETTING_NAMES))
    return f"  settings.dat: {states}"


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("uce", type=Path)
    parser.add_argument("--list", action="store_true",
                        help="list the cartridge files and the save folder")
    parser.add_argument("--extract", type=Path, metavar="DIR",
                        help="copy every file in the save folder to DIR")
    args = parser.parse_args()

    squash, squash_md5, zeros, save_md5, save = split_uce(args.uce.read_bytes())
    print(f"{args.uce.name}: squashfs {len(squash)} bytes"
          f" (multiple of 4096: {len(squash) % 4096 == 0})")
    print(f"  squashfs MD5 matches: {hashlib.md5(squash).digest() == squash_md5}")
    print(f"  trailer zero bytes ok: {zeros == bytes(32)}")
    print(f"  save MD5 matches: {hashlib.md5(save).digest() == save_md5}"
          " (False is normal once the game has been played)")
    if squash[:4] != b"hsqs":
        print("  warning: squashfs magic 'hsqs' not found")

    with tempfile.TemporaryDirectory() as temporary:
        image = Path(temporary) / "save.img"
        image.write_bytes(save)
        # The game's ./save folder is the overlay's upper/ directory.
        names = []
        for line in debugfs(image, "ls -p /upper").decode(errors="replace").splitlines():
            # ls -p prints /inode/mode/uid/gid/name/size/ records.
            parts = line.strip().strip("/").split("/")
            if len(parts) >= 6 and parts[4] not in (".", ".."):
                names.append((parts[4], parts[5]))
        print("  save folder: " + (", ".join(f"{n} ({s} bytes)" for n, s in names) or "empty"))
        files = {name: debugfs(image, f"cat /upper/{name}") for name, _ in names}
        print(describe_scores(files.get("hiscore.dat", b"")))
        print(describe_settings(files.get("settings.dat", b"")))
        if "probe.log" in files:
            print(f"  probe.log present ({len(files['probe.log'])} bytes); use --extract")

        if args.list:
            unsquashfs = find("unsquashfs", "squashfs")
            if unsquashfs:
                cart = Path(temporary) / "cart.squashfs"
                cart.write_bytes(squash)
                listing = subprocess.run([unsquashfs, "-lls", str(cart)],
                                         capture_output=True, text=True).stdout
                print("--- cartridge files\n" + listing.strip())
            else:
                print("(install squashfs to list the cartridge files)")

        if args.extract:
            args.extract.mkdir(parents=True, exist_ok=True)
            for name, data in files.items():
                (args.extract / name).write_bytes(data)
            print(f"  copied {len(files)} save file(s) to {args.extract}")


if __name__ == "__main__":
    try:
        main()
    except (ValueError, RuntimeError) as error:
        sys.exit(f"error: {error}")
