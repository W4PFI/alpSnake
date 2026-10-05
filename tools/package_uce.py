"""Package a recipe folder into a .UCE cartridge file (used by build.py).

A .UCE is what the AtGames cabinet loads from a USB stick. It is a single
file with this byte layout:

    offset 0       squashfs image of the recipe folder, zero-padded up to a
                   multiple of 4096 bytes          (read-only cartridge files)
    + padded size  16-byte MD5 of the padded squashfs
    + 16           32 zero bytes
    + 32           16-byte MD5 of the save image
    + 16           the save image: a 4 MiB ext4 filesystem (read/write)

The cabinet mounts the squashfs as the cartridge folder and mounts the ext4
image as an overlay on ./save, so files the game writes there (high scores,
settings.dat) persist between launches - inside the .UCE file itself.
Copying a freshly built .UCE over the old one therefore resets the save.

Also provides small helpers that build.py imports: ROOT, homebrew_tool(),
run().
"""

import hashlib
import shutil
import subprocess
from pathlib import Path


# The project folder; finished .UCE files go in dist/; the save image is 4 MiB.
ROOT = Path(__file__).resolve().parent.parent
OUTPUT = ROOT / "dist"
SAVE_SIZE = 4 * 1024 * 1024


def homebrew_tool(formula, name):
    """Return the path of `name` inside Homebrew formula `formula`, or None.

    Several needed tools (llvm's clang, lld, mksquashfs, mke2fs, debugfs) are
    "keg-only" or live in sbin, so they are often not on PATH even when
    installed; `brew --prefix <formula>` tells us where to look.
    """
    brew = shutil.which("brew")
    if not brew:
        return None
    result = subprocess.run([brew, "--prefix", formula], capture_output=True,
                            text=True, check=False)
    if result.returncode != 0:
        return None
    prefix = Path(result.stdout.strip())
    for directory in ("bin", "sbin"):
        candidate = prefix / directory / name
        if candidate.is_file():
            return str(candidate)
    return None


def tool(name):
    """Find a packaging tool on PATH or in its Homebrew formula (squashfs for
    mksquashfs, e2fsprogs for mke2fs/debugfs); raise if it is missing."""
    found = shutil.which(name)
    if found:
        return found
    formula = "squashfs" if name == "mksquashfs" else "e2fsprogs"
    homebrew = homebrew_tool(formula, name)
    if homebrew:
        return homebrew
    raise RuntimeError(f"Required build tool not found: {name}")


def run(*args):
    """Run a command, hiding its normal output; raise if it fails."""
    subprocess.run(args, check=True, stdout=subprocess.DEVNULL)


def make_save_image(path, workdir):
    """Create the blank 4 MiB ext4 save image at `path`.

    The cabinet uses this filesystem as the writable layer of an overlay
    mounted on the cartridge's ./save folder, so it contains the two
    directories overlayfs needs: upper/ (the files the game writes end up
    here) and work/ (overlayfs's scratch space). It starts with an empty
    upper/hiscore.dat, plus recipe/save/retroplayer.ini if the recipe has one
    (ALP Snake's does not).
    """
    save_root = workdir / "save_root"
    (save_root / "upper").mkdir(parents=True)
    (save_root / "work").mkdir()
    (save_root / "upper" / "hiscore.dat").touch()
    ini = workdir / "recipe" / "save" / "retroplayer.ini"
    if ini.is_file():
        shutil.copyfile(ini, save_root / "upper" / "retroplayer.ini")
    # Make a 4 MiB (sparse) file, then format it as ext4 in place, copying
    # save_root's contents in with -d (-q quiet, -F don't ask questions).
    with path.open("wb") as save_file:
        save_file.truncate(SAVE_SIZE)
    run(tool("mke2fs"), "-q", "-F", "-d", str(save_root), "-t", "ext4", str(path))
    # mke2fs -d copies our Mac user's ownership and permissions. Rewrite them
    # with debugfs ("sif" = set inode field) so the files belong to uid/gid 12
    # (the same id mksquashfs is told to use below) and are world-writable
    # (0777 dirs/file, 0666 for the ini), so the game can always write its saves
    # whatever user it runs as. The leading 040/0100 are the inode type bits
    # for directory/regular file.
    entries = [
        ("/upper", "040777"),
        ("/work", "040777"),
        ("/upper/hiscore.dat", "0100777"),
    ]
    if ini.is_file():
        entries.append(("/upper/retroplayer.ini", "0100666"))
    for image_path, mode in entries:
        for field, value in (("mode", mode), ("uid", "12"), ("gid", "12")):
            run(tool("debugfs"), "-w", "-R", f"sif {image_path} {field} {value}", str(path))


def package_recipe(recipe, workdir, name):
    """Build dist/<name> from the recipe folder (layout in the module docstring).

    workdir is a scratch folder for the intermediate images.
    """
    save_image = workdir / "save.img"
    make_save_image(save_image, workdir)
    squash_image = workdir / "cart.squashfs"
    # Pack the recipe read-only. -b 262144: 256 KiB compression blocks;
    # -force-uid/-gid 12: every file owned by id 12 (as for the save image);
    # -noappend: always start a new image; -nopad: we pad to 4 KiB ourselves
    # below; -no-xattrs: drop macOS extended attributes; -processors 2: limit
    # parallel compression threads.
    run(tool("mksquashfs"), str(recipe), str(squash_image), "-b", "262144",
        "-force-uid", "12", "-force-gid", "12", "-noappend", "-nopad",
        "-no-xattrs", "-processors", "2")
    squash = squash_image.read_bytes()
    # Round the squashfs up to the next multiple of 4096 with zero bytes.
    padded_squash = squash.ljust((len(squash) + 4095) // 4096 * 4096, b"\0")
    save = save_image.read_bytes()
    OUTPUT.mkdir(exist_ok=True)
    result = OUTPUT / name
    with result.open("wb") as output:
        # The UCE byte layout described at the top of this file.
        output.write(padded_squash)
        output.write(hashlib.md5(padded_squash).digest())
        output.write(bytes(32))
        output.write(hashlib.md5(save).digest())
        output.write(save)
    # Print the size and SHA-256 so the copy on the USB stick can be compared.
    digest = hashlib.sha256(result.read_bytes()).hexdigest()
    print(f"{result}: {result.stat().st_size} bytes, SHA-256 {digest}")
