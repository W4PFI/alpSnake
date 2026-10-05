#!/usr/bin/env python3
"""Build ALP Snake into a .UCE cartridge file for the AtGames Legends Pinball HD.

Run via ./build.sh (which checks the macOS prerequisites first). Steps:

1. Cross-compile two AArch64 Linux binaries with Homebrew's clang + lld:
   * the libretro core, build/mame2003_plus_libretro_custom_save.so, which
     Retroplayer (the cabinet's emulator front end) loads and drives, and
   * the backglass helper, build/alp_snake_backglass, a small SDL2 program
     the core starts to draw the second screen.
   Neither uses a C library (see the flag notes in build()).
2. Lay out a "recipe" folder - exactly the files the cabinet will see:

       recipe/
         exec.sh                 the launcher the cabinet runs (written below)
         cartridge.xml           title/description/boxart for the menu
         title.png -> boxart/boxart.png
         emu/
           mame2003_plus_libretro_custom_save.so   our core (MAME's name!)
           alp_snake_backglass                     backglass helper (0755)
           snake-font.bin                          smooth font atlas
           OFL-Nunito.txt                          font licence
         roms/
           snake.zip             a TEXT placeholder; Retroplayer needs some
                                 "content" path to hand to the core
           hiscore.dat           empty placeholder file
         boxart/
           boxart.png            menu art (make_snake_boxart.swift)
           addon.z.png           fully transparent bezel image
           snake-backglass.bmp   backglass art (make_backglass_art.swift)
         save/                   empty; ./save is an overlay at run time

3. Hand the recipe to package_uce.package_recipe(), which wraps it in a
   squashfs plus a blank save image to make dist/ALP-Snake.UCE.

Environment variables: CLANG / LINKER override the compiler and linker
paths; SDL2_LINK_LIBRARY links against a real libSDL2 instead of the stub;
ALP_SNAKE_PROBE=1 builds the diagnostic probe cartridge instead.
"""

import os
import shutil
import struct
import tempfile
import zlib
from pathlib import Path

from package_uce import ROOT, homebrew_tool, package_recipe, run


SOURCE = ROOT / "src"
TOOLS = ROOT / "tools"
ASSETS = ROOT / "assets"
BUILD = ROOT / "build"
# Optional path to a real aarch64 libSDL2 to link against. Normally unset, and
# we link against tools/sdl2_link_stub.c instead (only names matter to the
# linker; the cabinet supplies the real SDL2 at run time).
SDL2_LINK_LIBRARY = os.environ.get("SDL2_LINK_LIBRARY")
# ALP_SNAKE_PROBE=1 builds a diagnostic UCE that presents itself to
# Retroplayer like the cabinet's MAME 2003-Plus core, logs what Retroplayer
# does to our core (save/probe.log), and tries to copy Retroplayer's console
# output to the USB stick. It never copies any AtGames program or file.
# See README and DEVELOPER.md.
PROBE = os.environ.get("ALP_SNAKE_PROBE") == "1"
# The core and its placeholder content are named like the cabinet's MAME
# 2003-Plus AddOns so Retroplayer offers its full arcade menu (Insert Coin,
# Save Slots, Adv. Config). roms/snake.zip is a text placeholder, not a ROM.
CORE_NAME = "mame2003_plus_libretro_custom_save.so"
CONTENT_NAME = "snake.zip"
UCE_NAME = "ALP-Snake-Probe.UCE" if PROBE else "ALP-Snake.UCE"
TITLE = "ALP Snake Probe" if PROBE else "ALP Snake"


def write_transparent_bezel(path):
    """Write a 1280x720 PNG in which every pixel is fully transparent.

    exec.sh points Retroplayer's BezelPath at this file, so no bezel artwork
    is drawn over or around our full-screen game. The PNG is assembled by hand
    (no imaging library needed): signature, IHDR, one zlib-compressed IDAT,
    IEND. Each chunk is length + type + data + CRC-32 of type and data.
    """
    def chunk(kind, data):
        checksum = zlib.crc32(kind + data) & 0xffffffff
        return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", checksum)

    width, height = 1280, 720
    # IHDR: width, height, bit depth 8, colour type 6 (RGBA), compression 0,
    # filter method 0, no interlace.
    header = struct.pack(">IIBBBBB", width, height, 8, 6, 0, 0, 0)
    # Each scanline is a filter-type byte (0 = none) then width RGBA pixels,
    # all zero = transparent black.
    pixels = (b"\0" + bytes(width * 4)) * height
    path.write_bytes(
        b"\x89PNG\r\n\x1a\n"
        + chunk(b"IHDR", header)
        + chunk(b"IDAT", zlib.compress(pixels, 9))
        + chunk(b"IEND", b"")
    )


def compiler(environment_name, formula, name):
    """Find a build tool: $environment_name if set, else Homebrew's copy of
    `name` from `formula` (Apple's Xcode tools have no ld.lld for linking
    Linux ELF files), else whatever is on PATH."""
    override = os.environ.get(environment_name)
    if override:
        return override
    found = homebrew_tool(formula, name) or shutil.which(name)
    if found:
        return found
    raise RuntimeError(f"Set {environment_name} to the path of {name}")


def build():
    """Compile both binaries, assemble the recipe folder and package the UCE."""
    clang = compiler("CLANG", "llvm", "clang")
    linker = compiler("LINKER", "lld", "ld.lld")
    # The artwork scripts run with `swift`; point the clang/Swift module caches
    # at a known-writable folder in the temp directory (unless already set).
    cache = Path(tempfile.gettempdir()) / "alp-snake-swift-cache"
    cache.mkdir(exist_ok=True)
    os.environ.setdefault("CLANG_MODULE_CACHE_PATH", str(cache))
    os.environ.setdefault("SWIFT_MODULE_CACHE_PATH", str(cache))

    BUILD.mkdir(exist_ok=True)
    if SDL2_LINK_LIBRARY:
        sdl2 = Path(SDL2_LINK_LIBRARY)
        if not sdl2.is_file():
            raise RuntimeError(f"SDL2_LINK_LIBRARY does not exist: {sdl2}")
    else:
        # Build the link-only stub SDL2 (see tools/sdl2_link_stub.c).
        #   --target=aarch64-linux-gnu  cross-compile for the cabinet's CPU/OS
        #   -fPIC -shared               it is a shared library
        #   -nostdlib                   no libc or start files to link in
        #   --ld-path                   use Homebrew's ld.lld: macOS's own ld
        #                               cannot produce Linux ELF files
        #   -soname libSDL2-2.0.so.0    the name recorded as NEEDED in our
        #                               binaries, i.e. the real SDL2's soname
        #                               the cabinet's loader will look up
        sdl2 = BUILD / "libSDL2-link-only.so"
        run(
            clang, "--target=aarch64-linux-gnu", "-std=gnu11", "-fPIC",
            "-shared", "-nostdlib", "-O2", str(TOOLS / "sdl2_link_stub.c"),
            "-o", str(sdl2), f"--ld-path={linker}",
            "-Wl,-soname,libSDL2-2.0.so.0",
        )
    # The smooth font atlas is pre-rendered by tools/make_smooth_font.swift:
    # "SNK1" magic then 37 glyphs (A-Z, 0-9, space) of 128x144 alpha bytes.
    font_atlas = ASSETS / "snake-font.bin"
    if (not font_atlas.is_file() or font_atlas.stat().st_size != 4 + 37 * 128 * 144
            or font_atlas.read_bytes()[:4] != b"SNK1"):
        raise RuntimeError("Font atlas is invalid; regenerate assets/snake-font.bin")

    core = BUILD / CORE_NAME
    helper = BUILD / "alp_snake_backglass"
    # The libretro core: a shared object Retroplayer dlopen()s.
    #   -std=gnu11          C11 plus GNU extensions (inline asm for svc #0)
    #   -fPIC -shared       position-independent shared library, as dlopen needs
    #   -nostdlib           link no libc and no crt start files: the core makes
    #                       its own system calls, so it cannot clash with the
    #                       firmware's libc version
    #   -ffreestanding      tell the compiler there is no hosted C library, so
    #                       it must not assume standard functions exist
    #   -fno-builtin        don't treat names like memcpy/strlen specially
    #                       (the code may define its own versions)
    #   -O2                 optimise
    #   -DALP_SNAKE_PROBE   (probe build only) compile in Retroplayer call logging
    #   -I include, -I src  include/libretro.h and our own headers
    #   sdl2                the stub (or real) SDL2, for SDL_PushEvent
    #   -Wl,--no-undefined  fail the LINK if any symbol is unresolved. Shared
    #                       libraries normally allow undefined symbols and only
    #                       fail when loaded; with no libc, an accidental call
    #                       to memcpy/memset (e.g. emitted by the compiler for a
    #                       big struct copy) would otherwise only show up as a
    #                       core that silently fails to load on the cabinet.
    run(
        clang, "--target=aarch64-linux-gnu", "-std=gnu11", "-fPIC", "-shared",
        "-nostdlib", "-ffreestanding", "-fno-builtin", "-O2",
        *(["-DALP_SNAKE_PROBE"] if PROBE else []),
        f"-I{ROOT / 'include'}", f"-I{SOURCE}",
        str(SOURCE / "snake_game.c"), str(SOURCE / "snake_smooth_font.c"),
        str(SOURCE / "snake_audio.c"),
        str(SOURCE / "snake_libretro.c"), str(sdl2), "-o", str(core),
        f"--ld-path={linker}", "-Wl,--no-undefined",
    )
    # The backglass helper: a normal dynamically linked executable whose entry
    # point is _start() in snake_backglass.c (no libc, so no main()).
    #   -fno-builtin -nostdlib  as above: no libc, no crt start files
    #   -fno-stack-protector    the stack protector calls __stack_chk_fail and
    #                           reads a guard set up by libc; neither exists
    #   -no-pie                 a plain fixed-address executable (ET_EXEC)
    #                           rather than a position-independent one; the
    #                           helper gains nothing from PIE and this keeps
    #                           its startup as simple as possible
    #   snake_smooth_font.c     shared with the core: loads the font atlas
    #   sdl2                    satisfies the SDL2 names; NEEDED libSDL2-2.0.so.0
    #   --dynamic-linker=/lib/ld-linux-aarch64.so.1
    #                           the program interpreter written into the ELF.
    #                           Because the helper needs a shared library
    #                           (SDL2), the kernel must start it through the
    #                           system's dynamic loader, which maps SDL2 (and its
    #                           own deps) and then jumps to _start. This is the
    #                           standard aarch64 glibc loader path.
    #   -Wl,--no-undefined      catch any missing symbol at build time
    run(
        clang, "--target=aarch64-linux-gnu", "-std=gnu11", "-O2",
        "-fno-builtin", "-fno-stack-protector", "-nostdlib", "-no-pie",
        f"-I{SOURCE}", str(SOURCE / "snake_smooth_font.c"),
        str(SOURCE / "snake_backglass.c"), str(sdl2),
        "-o", str(helper), f"--ld-path={linker}",
        "-Wl,--dynamic-linker=/lib/ld-linux-aarch64.so.1", "-Wl,--no-undefined",
    )

    # Assemble the recipe folder (layout in the module docstring) in a
    # throwaway temp directory, then package it.
    with tempfile.TemporaryDirectory(prefix="alp-snake-uce-") as temporary:
        workdir = Path(temporary)
        recipe = workdir / "recipe"
        for directory in ("emu", "roms", "boxart", "save"):
            (recipe / directory).mkdir(parents=True)
        shutil.copyfile(core, recipe / "emu" / core.name)
        shutil.copyfile(helper, recipe / "emu" / helper.name)
        shutil.copyfile(font_atlas, recipe / "emu" / font_atlas.name)
        shutil.copyfile(ASSETS / "OFL-Nunito.txt", recipe / "emu" / "OFL-Nunito.txt")
        (recipe / "emu" / helper.name).chmod(0o755)
        # Retroplayer insists on a content file; our core ignores its contents.
        (recipe / "roms" / CONTENT_NAME).write_text("Original ALP Snake game\n")
        (recipe / "roms" / "hiscore.dat").touch()
        write_transparent_bezel(recipe / "boxart" / "addon.z.png")

        boxart = BUILD / "boxart.png"
        run("swift", str(TOOLS / "make_snake_boxart.swift"), str(boxart),
            str(ASSETS / "snake-mascot.png"))
        shutil.copyfile(boxart, recipe / "boxart" / "boxart.png")
        # title.png is the same picture; a relative symlink inside the squashfs.
        (recipe / "title.png").symlink_to("boxart/boxart.png")

        backglass = BUILD / "backglass.png"
        run("swift", str(TOOLS / "make_backglass_art.swift"),
            str(ASSETS / "snake-mascot.png"), str(backglass))
        # SDL2 alone loads only BMP, so convert with macOS's sips. Going through
        # JPEG first drops the alpha channel, giving a plain opaque BMP.
        opaque_artwork = workdir / "backglass.jpg"
        run("sips", "-s", "format", "jpeg", "-s", "formatOptions", "best",
            str(backglass), "--out", str(opaque_artwork))
        run("sips", "-s", "format", "bmp", str(opaque_artwork),
            "--out", str(recipe / "boxart" / "snake-backglass.bmp"))

        # Metadata the cabinet's menu reads (title, description, boxart).
        (recipe / "cartridge.xml").write_text(
            '<?xml version="1.0" encoding="UTF-8"?>\n'
            '<byog_cartridge version="1.0">\n'
            f'  <title>{TITLE}</title>\n'
            '  <desc>Original dual-screen Snake game for Legends Pinball HD</desc>\n'
            '  <boxart file="boxart/boxart.png" ext="png">\n'
            '</byog_cartridge>\n'
        )
        # exec.sh is what the cabinet runs to start the cartridge, from inside
        # the mounted cartridge folder (so ./emu, ./roms, ./save work).
        #   cp ...addon.z.png      put the transparent bezel where gameinfo
        #                          can point at it
        #   printf ... gameinfo    /tmp/gameinfo.ini tells Retroplayer how to
        #                          lay out the screen: the bezel image, and a
        #                          1080x1920 (portrait) game area at 0,0, i.e.
        #                          the whole playfield screen
        #   cleanup / trap ... 0   delete both temp files when the script exits
        #                          (trap on signal 0 = shell exit), so they don't
        #                          leak into the next game the cabinet launches
        launch = recipe / "exec.sh"
        start = (
            "#!/bin/sh\n"
            "cp ./boxart/addon.z.png /tmp/addon.z.png\n"
            "printf '[Property]\\nBezelPath=/tmp/addon.z.png\\n"
            "Width=1080\\nHeight=1920\\nX=0\\nY=0\\n' > /tmp/gameinfo.ini\n"
            "cleanup() { rm -f /tmp/gameinfo.ini /tmp/addon.z.png; }\n"
            "trap cleanup 0\n"
        )
        # Start the cabinet's own Retroplayer with our core and placeholder
        # content. It runs in the foreground; exec.sh ends when the game does.
        player = f"/emulator/retroplayer ./emu/{CORE_NAME} ./roms/{CONTENT_NAME}"
        if PROBE:
            # Save Retroplayer's console output and some basic system facts to
            # an alp-probe folder on the USB stick (found the way RetroFE
            # does). On our cabinet no /media/usbN mount was found, so this
            # branch never ran; save/probe.log in the save area is the
            # reliable route (tools/inspect_uce.py reads it).
            launch.write_text(
                start
                + "usb=$(df -P | awk '{print $6}' | grep -m1 '^/media/usb[0-9]')\n"
                "out=\"$usb/alp-probe\"\n"
                "if [ -n \"$usb\" ] && mkdir -p \"$out\"; then\n"
                "  ls -la /emulator /tmp > \"$out/listing.txt\" 2>&1\n"
                "  cp /tmp/gameinfo.ini \"$out/\" 2>/dev/null\n"
                "  { id; uname -a; cat /proc/version; } > \"$out/system.txt\" 2>&1\n"
                f"  {player} > \"$out/retroplayer-output.txt\" 2>&1\n"
                "  cp ./save/probe.log \"$out/\" 2>/dev/null\n"
                "  sync\n"
                "else\n"
                f"  {player}\n"
                "fi\n"
            )
        else:
            launch.write_text(start + player + "\n")
        launch.chmod(0o755)
        package_recipe(recipe, workdir, UCE_NAME)


if __name__ == "__main__":
    build()
