# Building a game for the AtGames Legends Pinball HD: a developer's guide

ALP Snake is two things. It is a game, and it is a worked example for anyone
who wants to write their own original game for the **original AtGames Legends
Pinball HD** (firmware 5.70.0). This guide collects what we learned while
building it: what the cabinet expects, what works, what doesn't, and why the
code is shaped the way it is.

The source is heavily commented with the same goal. Read this guide for the
big picture, then read the code. A good order is `snake_game.h`, then
`snake_libretro.c` (start with its header and `retro_run()`), then
`snake_audio.c` and `snake_backglass.c`.

> Everything here was found by experiment on one cabinet with one firmware
> version. Other models or firmware may behave differently. Facts marked
> *observed* are things we saw on the machine rather than anything AtGames
> documents.

---

## Where this knowledge came from

**There is no official AtGames developer guide or SDK for the Legends
cabinets** that we could find. AtGames' own [FAQ](https://arcades.atgames.net/faq/)
confirms the BYOG AddOn feature: you can sideload AddOn games you own over
USB, packaged as UCE files. It does not document how to make them. For
questions about the cabinet itself, contact
[AtGames support](https://www.atgames.net/arcades/contact/).

Everything in this guide came from public sources, open standards, and
watching **our own code** run on a cabinet we own:

| Source | What it gave us |
|---|---|
| [libretro API](https://docs.libretro.com/) and its open `libretro.h` header (included in `include/`) | The core interface: `retro_run`, video, audio, input, environment calls |
| [FalkensMaze1983/ultimate_addon](https://github.com/FalkensMaze1983/ultimate_addon) (community) | How AddOn UCEs are laid out: `exec.sh`, `cartridge.xml`, running `/emulator/retroplayer` with a core and content, `gameinfo.ini` bezels |
| [jimnarey/alu_auto_builder](https://github.com/jimnarey/alu_auto_builder) (community) | A second open implementation of UCE building, including the save area |
| [Wagner's TechTalk ALP guide](https://wagnerstechtalk.com/alp/) and [ALP MAME guide](https://wagnerstechtalk.com/alpmame/) (community) | How owners use BYOG/AddOn and CoinOpsX on the Legends Pinball |
| [LegendsUnchained](https://github.com/LegendsUnchained) and [VBitsHub/ALU_retrofeCustomCores](https://github.com/VBitsHub/ALU_retrofeCustomCores) (community) | Public examples of other Legends homebrew projects; the RetroFE scripts showed how to find the USB stick |
| [libretro/mame2003-plus-libretro](https://github.com/libretro/mame2003-plus-libretro) (open source) | The MAME 2003-Plus core's public name, version string and option names, which we copy so the cabinet shows its arcade menu |
| The Linux kernel's public [syscall](https://man7.org/linux/man-pages/man2/syscalls.2.html) and [evdev input](https://docs.kernel.org/input/input.html) documentation | System calls, the monotonic clock, reading the trackball |
| A UCE the cabinet's owner already had (a MAME AddOn) | Looking at its *packaging* (file names, `exec.sh`, `cartridge.xml`) to confirm the conventions. None of its contents were copied into this project |
| Our own probe build, run on our own cabinet | A log of the calls Retroplayer makes **into our core**, written to our own save area |

What we did **not** do:

- **Firmware and root.** We didn't modify, extract, decrypt or patch the
  firmware. We didn't get root or a shell on the cabinet, or use any exploit
  or jailbreak.
- **Protections.** We didn't get around any copy protection, licence check
  or security measure. The game runs through the cabinet's normal,
  user-facing BYOG → AddOn menu.
- **What ships.** We ship no AtGames code, firmware, artwork or other assets,
  and no copy of Retroplayer or the cabinet's SDL2. At run time the game uses
  the SDL2 already installed on the machine.
- **Retroplayer.** We didn't disassemble or decompile it. An early probe
  build tried to copy it to the USB stick for study. That copy never
  happened, because the stick's mount point wasn't found, and the line has
  since been removed. Everything we learned about Retroplayer comes from
  what it does to our own core.
- **ROMs and game content.** We ship no ROMs or third-party game content.
  `roms/snake.zip` is a one-line text file; the whole game is original code,
  art and music.

This is an unofficial project, not affiliated with or endorsed by AtGames;
all trademarks belong to their owners (see the README's
[Trademarks and disclaimer](README.md#trademarks-and-disclaimer)).

Making software work with a device you own by observing its public
interfaces is the usual way homebrew is made. If you plan to publish or sell
something, check the terms that apply to you; this guide is not legal advice.

---

## Contents

- [Where this knowledge came from](#where-this-knowledge-came-from)
1. [The big picture](#1-the-big-picture)
2. [The cartridge: UCE files](#2-the-cartridge-uce-files)
3. [Building without a C library](#3-building-without-a-c-library)
4. [The libretro core and the cabinet menu](#4-the-libretro-core-and-the-cabinet-menu)
5. [Video](#5-video)
6. [Timing](#6-timing)
7. [Sound](#7-sound)
8. [Input: flippers, buttons and the trackball](#8-input-flippers-buttons-and-the-trackball)
9. [The backglass](#9-the-backglass)
10. [Saving data](#10-saving-data)
11. [Settings (DIP switches)](#11-settings-dip-switches)
12. [Diagnostics on the real machine](#12-diagnostics-on-the-real-machine)
13. [Testing without the cabinet](#13-testing-without-the-cabinet)
14. [Pitfalls we hit](#14-pitfalls-we-hit)
15. [Checklist for a new game](#15-checklist-for-a-new-game)
16. [Source map](#16-source-map)

---

## 1. The big picture

```
 USB stick
 └── ALP-Snake.UCE ──────────── one file: read-only cartridge + writable save area
        │
        ▼   (BYOG → AddOn → ALP Snake)
 cabinet firmware mounts it, runs exec.sh
        │
        ▼
 /emulator/retroplayer ./emu/<core>.so ./roms/snake.zip
        │  libretro API: retro_run() ~60×/s, video, audio, input
        ▼
 snake_libretro.c  (our "core": a shared library)
   ├── snake_game.c       rules: snake, food, mouse, sign, hawk (no hardware)
   ├── snake_audio.c      software synthesizer and music
   ├── draws the 1080×1920 playfield
   ├── reads the trackball from /dev/input directly
   ├── writes ./save/hiscore.dat and ./save/settings.dat
   └── starts ──────────► alp_snake_backglass (separate SDL2 program)
                            draws the 1920×1080 backglass,
                            reads /tmp/alp-snake-state.bin
```

The cabinet has a "Bring Your Own Game" (BYOG) AddOn feature meant for arcade
ROMs. The cabinet runs AddOns with **Retroplayer**, a libretro frontend. So the
easiest way in for an original game is to *be a libretro core*. Retroplayer
loads the core, calls `retro_run()` every frame, and takes care of the
display, the speakers and the controls.

The playfield screen is the libretro video output. The **backglass is not**,
so a second, separate program draws it.

You don't need root access, a firmware change or any third-party loader.

---

## 2. The cartridge: UCE files

A `.UCE` is a single file:

| Offset | Content |
|---|---|
| 0 | squashfs image of the "recipe" folder, zero-padded to a multiple of 4096 |
| + padded size | 16-byte MD5 of the padded squashfs |
| + 16 | 32 zero bytes |
| + 32 | 16-byte MD5 of the save image |
| + 16 | the save image: a 4 MiB ext4 filesystem |

`tools/package_uce.py` builds it. `tools/inspect_uce.py` checks one and shows
what is in its save area (see [§12](#12-diagnostics-on-the-real-machine)).

**The recipe folder** (built by `tools/build.py`):

```
exec.sh                 the launcher the cabinet runs (must be executable)
cartridge.xml           title, description and box art for the AddOn menu
title.png -> boxart/boxart.png
emu/  mame2003_plus_libretro_custom_save.so    our core
      alp_snake_backglass                      backglass helper (executable)
      snake-font.bin, OFL-Nunito.txt           font atlas and its licence
roms/ snake.zip         a TEXT placeholder; Retroplayer needs "content"
      hiscore.dat
boxart/ boxart.png, addon.z.png (transparent bezel), snake-backglass.bmp
```

**exec.sh** does three things:

```sh
cp ./boxart/addon.z.png /tmp/addon.z.png
printf '[Property]\nBezelPath=/tmp/addon.z.png\nWidth=1080\nHeight=1920\nX=0\nY=0\n' > /tmp/gameinfo.ini
/emulator/retroplayer ./emu/<core>.so ./roms/snake.zip
```

Retroplayer reads `/tmp/gameinfo.ini` for the bezel image and for the video
rectangle. The working directory is the cartridge folder, so relative paths
like `./emu/...` and `./save/...` work.

**The save area.** The ext4 image holds `upper/` and `work/` directories
owned by uid/gid 12. The cabinet mounts it as an overlay on the cartridge's
`./save`, so anything the game writes to `./save/...` ends up inside the
`.UCE` itself.

Some consequences:
- Copying a freshly built `.UCE` over a played one **wipes the saves**.
  `build-and-copy.command` backs up the old one to `dist/` first.
- The cabinet does **not** update the save MD5 after writing (*observed*), and
  loads the file anyway. A mismatch on a played cartridge is normal.
- To see what a game saved on the machine, copy the `.UCE` back to the Mac and
  run `tools/inspect_uce.py` on it.

---

## 3. Building without a C library

Both programs are built **freestanding**: `-nostdlib -ffreestanding
-fno-builtin`, cross-compiled for AArch64 Linux with clang and lld from
Homebrew.

Why freestanding? The core is a shared library loaded into Retroplayer's
process. Linking against our own libc, or assuming a particular libc version
on the cabinet, invites trouble. Without libc we have no surprises and no
version dependencies.

What that means in practice:

- **System calls by hand.** Files, clocks, processes and input devices are
  reached with the AArch64 `svc #0` instruction. The call number goes in `x8`
  and the arguments in `x0`–`x5`. The `system_call()` wrappers live in
  `snake_libretro.c` and `snake_backglass.c` (`font_call()` in
  `snake_smooth_font.c`). The numbers and flags are named in
  `src/alp_syscall.h`.
- **arm64 is not x86.** arm64 uses the generic syscall table, which has no
  `open`, `rename` or `unlink`; you use `openat`, `renameat` and `unlinkat`
  with `AT_FDCWD`. Some flag values differ too. `O_DIRECTORY` is `0x4000` on
  arm64, while x86's `0x10000` means `O_DIRECT` on arm64. Take values from the
  arm64 headers.
- **No hidden memcpy/memset.** The compiler emits calls to `memset` and
  `memcpy` for things like `big_array = {0}` or copying a large struct. Those
  functions don't exist here, so the link fails (we link with
  `-Wl,--no-undefined` to catch this at build time rather than on the
  cabinet). The cures are:
  - declare big buffers `static`, so the loader zeroes them;
  - copy with small loops;
  - keep structs that get copied small.
- **No libm.** The sine table, the filter coefficients and the decay curves in
  the synthesizer are computed with plain arithmetic (see
  [§7](#7-sound)).
- **The backglass helper is an executable**, so it has no `main()`. It starts
  at `_start` and ends with the `exit` syscall. It is linked with
  `--dynamic-linker=/lib/ld-linux-aarch64.so.1` so the system loader can pull
  in the cabinet's SDL2.
- **SDL2 without SDL.** We don't ship SDL and don't need its headers. The few
  functions used are declared by hand, and `tools/sdl2_link_stub.c` builds a
  link-only `libSDL2-2.0.so.0` with the right soname. At run time the
  cabinet's real SDL2 is used. The core uses exactly one SDL function,
  `SDL_PushEvent`, which resolves to the copy Retroplayer has already loaded.

`tools/build.py` explains every compiler flag in comments.

---

## 4. The libretro core and the cabinet menu

### The lifecycle

```
retro_set_environment → retro_init → retro_get_system_info
→ retro_load_game → retro_get_system_av_info
→ retro_run (every frame) ... → retro_reset (sometimes) ...
→ retro_unload_game → retro_deinit
```

`retro_load_game` does the real start-up: pixel format, core variables, loading
settings and scores, starting attract mode and launching the backglass helper.
`retro_run` is the heartbeat. Its steps are listed in the comment above it in
`snake_libretro.c`.

### Pretending to be MAME 2003-Plus

Retroplayer's in-game menu (opened from the cabinet) is much richer for its
MAME core. It adds **Insert Coin**, **Save Slots**, **Adv. Config** and
**Trackball resolution**. Any other core gets only Quit, Button Mapping,
Display Mode and Scanlines (*observed*). So ALP Snake presents itself as MAME
2003-Plus:

- the core file is named `mame2003_plus_libretro_custom_save.so`, like the
  cabinet's own MAME AddOns;
- `retro_get_system_info` reports `library_name = "MAME 2003-Plus"`, MAME's
  version string, `valid_extensions = "zip"`, `need_fullpath = true` and
  `block_extract = true`;
- the content is `roms/snake.zip`. It is only a text placeholder and the core
  ignores it;
- `retro_load_game` sends MAME's 33 core variables with `SET_VARIABLES`.

What each menu entry then does (*observed*):

| Menu entry | What reaches the core | How ALP Snake uses it |
|---|---|---|
| Insert Coin | the Select button | starts a game |
| Adv. Config | variable `mame2003-plus_display_setup` becomes `"enabled"` (announced by `GET_VARIABLE_UPDATE`) and R3 (button 15) is tapped for 4 frames | opens our own DIP-switch page |
| closing the menu | `retro_reset()`, then the variable goes back to `"disabled"` and R3 is tapped again | closes the page; the reset is ignored for 10 s afterwards |
| Save Slots | `retro_serialize` is called every 5 frames if `retro_serialize_size() > 0` | **not supported**: slots don't save even for real MAME games, so `retro_serialize_size` returns 0 |
| Display Mode | handled by Retroplayer | "Fill" can't be made to stick between launches |

More cabinet facts:
- **Core options aren't shown.** The cabinet doesn't show core options (v1 or
  otherwise). Asking for a variable it doesn't know returns a default instead
  of failing. A game therefore has to store its own settings (see
  [§11](#11-settings-dip-switches)).
- **`retro_reset()` is mostly ignored.** `retro_reset()` arrives whenever the
  cabinet menu closes, because MAME needs a restart after DIP changes. ALP
  Snake applies settings live, so it ignores the reset around menu use and
  never throws away a game in progress because of it.

The `ALP_SNAKE_PROBE` build (see [§12](#12-diagnostics-on-the-real-machine))
is how all of this was discovered. It logs every environment call, variable
change and button press.

---

## 5. Video

- **The screen.** The playfield is **1080 × 1920 portrait**. The core uses
  `RETRO_PIXEL_FORMAT_RGB565`, which takes 2 bytes per pixel, so a frame is
  about 4 MB.
- **Two frame buffers.** `draw_game()` paints the back buffer, then swaps.
  Retroplayer always receives a finished picture.
- **Redraw only when something changed.** A global `frame_dirty` flag is set
  by anything visible: a move, an animation tick, a popup or a menu change. On
  frames where nothing changed, the core hands Retroplayer the same buffer
  again, which costs nearly nothing. Full redraws on this CPU take a
  noticeable part of the 16.7 ms frame budget. The diagnostics overlay shows
  the worst drawing time (`DRAW`).
- **Text.** There are two fonts:
  - a 5×7 bitmap font (`snake_font.h`: A–Z, 0–9, `: - /`) for small labels;
  - an anti-aliased Nunito atlas (`assets/snake-font.bin`, made once by
    `tools/make_smooth_font.swift`) for titles and scores. The atlas has only
    A–Z, 0–9 and space, so a dash is drawn as a filled rectangle.
  - Load the atlas **before** measuring text. Otherwise the first frame
    centres titles using the fallback font's widths.
- **Fixed-point graphics.** Blending, the hawk's shadow and the confetti use
  integer and fixed-point maths. There is no floating point in the drawing
  path.
- **Bezel and the area around the game.** The game's video rectangle comes
  from `gameinfo.ini`. In the cabinet's default (centred) display mode,
  Retroplayer draws a bezel image around it. We point `BezelPath` at a fully
  transparent PNG so the cabinet's own background shows through. The core
  can't paint outside its rectangle.

---

## 6. Timing

This was the most important lesson: **don't measure time in frames.**
Retroplayer calls `retro_run()` about 60 times a second, but not exactly. A
slow draw, a background process or a menu pause all stretch frames. A game
that moves "every 8 frames" slows down whenever the cabinet is busy.

### Movement on the monotonic clock

The snake moves on deadlines taken from `clock_gettime(CLOCK_MONOTONIC)`, a
clock that never jumps:

```c
advance = now + HALF_FRAME_NS >= next_move_ns;   /* nearest-frame rule */
...
next_move_ns = deadline + interval;               /* schedule from the deadline */
if (speed changed || we fell more than half an interval behind)
    next_move_ns = now + interval;                /* restart, don't catch up */
```

- **Nearest frame.** A move fires on the frame closest to its deadline. That
  allows up to half a frame early (`HALF_FRAME_NS`, 8.33 ms), which beats
  being almost a whole frame late.
- **Schedule from the previous deadline, not from "now".** This keeps small
  jitter from building up into drift.
- **Never catch up with several moves at once.** After a stall, the next move
  is rescheduled instead.
- **Resume gap.** If more than `RESUME_GAP_NS` (250 ms) passes between frames,
  Retroplayer was paused (the cabinet menu, for example). The player gets a
  full interval before the next move, not an instant one.
- **Fallback.** If the clock can't be read, the code falls back to counting
  frames.

**Speed levels.** There are five, from 8 frames per move down to 4, expressed
as nanoseconds (`period × 1e9 / 60`). The level goes up every 5 foods, and a
SLOW sign drops it one level.

### Measure the real frame rate

Don't assume 60.00 Hz. The diagnostics overlay measures it; our cabinet shows
`FPS 5994`, which is 59.94 Hz. Audio depends on this, as the next section
explains.

---

## 7. Sound

The cabinet has good stereo speakers. Retroplayer uses the libretro **push**
model: each `retro_run()` hands over a batch of 16-bit stereo samples at
44.1 kHz.

### How many samples to push

The obvious approach is `44100 / 60 = 735` samples per frame. It is wrong
twice over:

1. **It drifts.** The real frame rate is 59.94 Hz, not 60.
2. **Slow frames lose sound.** When a frame takes 30 ms, 735 samples cover
   only 16.7 ms, and the rest is a gap. In ALP Snake that made the game-over
   sound choppy in long games, because longer games have more on screen and
   slower frames.

The fix (`audio_frames_due()`): each frame, push exactly as many samples as
real time has covered since the last frame:

```c
scaled  = (now - audio_clock_ns) * 44100 + audio_remainder;
frames  = scaled / 1e9;          /* whole samples due */
audio_remainder = scaled % 1e9;  /* carry the fraction: nothing is ever lost */
```

There is a cap of a quarter second (`AUDIO_MAX_FRAMES`, 11025 samples), so a
slow frame never drops audio. A longer gap is treated as a pause and the
clock simply restarts.

> **Rule:** sound is generated from the clock, not from the frame count. The
> music tempo, too, is counted in **samples**, so it stays exact whatever the
> video does.

### A synthesizer instead of samples

`snake_audio.c` is a small software synthesizer. Making sounds from code
keeps the cartridge tiny and needs no audio files or decoders.

**Per voice:**

```
oscillator (sine/square/saw/triangle/noise, glide, vibrato)
  → state-variable filter (low- or high-pass, resonance, "wah" sweep)
  → drive (soft distortion) → envelope → equal-power pan → echo send
```

**Master stage:** a ping-pong stereo echo, a soft clipper, then conversion to
int16.

- **Voices.** There are 32. A new sound takes a free voice, or steals the
  oldest one.
- **No libm:**
  - the sine table is filled once by repeatedly rotating a vector;
  - the note frequencies come from a one-octave table plus doubling and
    halving;
  - the filter coefficient and the exponential decay use first-order
    approximations;
  - `tanh` (the soft clip) is replaced by a Padé rational curve.
  - Sample timing is all integer counts, so it never drifts.
- **Sound effects are "recipes".** A recipe is a few lines of code that start
  voices with a waveform, a pitch, delays (for arpeggios), a filter sweep and
  an envelope.
  - The Pac-Man-style "waka" while moving alternates up and down sweeps,
    pitched a little higher at each speed level.
  - The game-over sound is a crash followed by a "wah wah wah waaah" trombone
    made with a resonant filter sweep.
- **Music.** A 16-step sequencer plays an original "Egyptian trap" song (E
  Hijaz scale, 140 BPM, 16 bars, looping back to bar 4).
  - One sixteenth note is `MUSIC_STEP_SAMPLES` = 4725 samples. The sequencer
    counts samples, not frames, which fixed the "jittery" music we first
    heard.
  - A fader fades the music in and out. Starting a game stops sound effects
    but leaves the music voices alone.
- **Mixing headroom.** Keep the sum well below int16 full scale (the code
  mixes towards ±30000) and let the soft clipper catch peaks. Hard clipping
  sounds terrible on these speakers.

### Quick checks

- Render sounds to WAV on the computer (the host preview harnesses did this)
  and listen before building a UCE.
- On the cabinet, the diagnostics overlay's `SND` value shows the percentage
  of offered samples Retroplayer accepted. It should read 100.

---

## 8. Input: flippers, buttons and the trackball

### Joypad (libretro)

| Cabinet control | libretro joypad id |
|---|---|
| Left flipper | L2 |
| Right flipper | R |
| Start | START |
| Joystick | UP/DOWN/LEFT/RIGHT |
| Insert Coin (menu) | SELECT |

Register input descriptors and controller info so the cabinet's Button Mapping
screen shows sensible names. React to button presses on edges (pressed now,
not pressed last frame), not while a button is held.

### The trackball (read directly)

We found no way to get the trackball's motion through libretro's input
callbacks on this cabinet, so the core reads it from the kernel:

1. Scan `/dev/input/event*` with `openat(O_RDONLY | O_NONBLOCK | O_CLOEXEC)`.
2. Ask each device which relative axes it has, with
   `ioctl(EVIOCGBIT(EV_REL, 8))` (`0x80084522`). Keep the first device that
   reports `REL_X` and `REL_Y`.
3. Each frame, `read()` all pending `struct input_event` records. Because the
   device is non-blocking, `read()` returns at once when there are none.
4. Add up the X and Y motion. When one axis passes a threshold (35), turn it
   into a direction. Let the totals decay so old motion fades.

The device is only read. No grabs and no settings changes, so the cabinet's
own use of it is unaffected.

---

## 9. The backglass

The backglass is a second display (1920 × 1080). Retroplayer gives the core
only the playfield, so ALP Snake starts **a second program** for it.

### Launching the helper from the core

These steps are in `start_backglass()`:

1. `clone(SIGCHLD)`, which is a plain fork.
2. In the child:
   - call `prctl(PR_SET_PDEATHSIG, SIGTERM)`, so the helper dies if
     Retroplayer dies;
   - check that `getppid()` is still the parent (it might have died
     already);
   - close every inherited file descriptor by listing `/proc/self/fd`, so the
     helper doesn't hold Retroplayer's files, sockets or devices open;
   - `execve("./emu/alp_snake_backglass")` with a minimal environment.
3. The environment variable **`ForceConnectID=93`** makes the cabinet's SDL2
   open the backglass panel (display connector 93) instead of the playfield
   (*observed*; it is cabinet-specific).
4. On unload, the core deletes the state file. It also sends SIGTERM and reaps
   the child with `wait4(WNOHANG)`, retrying for up to 200 ms.

### Talking to the helper: an atomic state file

- **Writing.** The core writes a tiny struct (`snake_backglass_state.h`) to
  `/tmp/alp-snake-state.tmp`, then renames it over
  `/tmp/alp-snake-state.bin`.
  - **`rename()` is atomic**, so the helper never reads a half-written file.
  - `/tmp` is RAM, so this is cheap.
  - The core writes when something changed and at least 10 times a second.
- **Reading.** The helper polls the file about 30 times a second.
  - It checks a magic number (and with it the layout version).
  - It redraws only when a value it shows changes, plus every 2 s and while
    confetti is falling.
  - When the file disappears, it exits.

A file is simpler than pipes or sockets. Either side can restart, nothing
blocks, and you can inspect it with `hexdump` while debugging.

### Drawing on the backglass

The helper uses SDL2's renderer: one full-screen window and a background
texture. The artwork is loaded from a BMP, because SDL can load BMPs without
extra libraries. The build converts it via JPEG so it has no alpha.

- **Font.** The Nunito glyphs are uploaded once as alpha textures and tinted
  with `SDL_SetTextureColorMod`.
- **Exiting.** The helper calls `SDL_Quit()` and then the `exit` syscall. It
  doesn't call `exit_group`, because the helper is single-threaded once SDL
  has shut down.

### Keeping the cabinet awake

The core reads the trackball behind SDL's back, so the firmware may see no
input during long attract sessions and dim or idle. Every ~20 s the core
pushes a fake **F24** key press and release with `SDL_PushEvent`. No game uses
F24. The event struct is built by hand, because we have no SDL headers.

---

## 10. Saving data

The rules for anything in `./save`:

1. **Write atomically.** Write `name.tmp`, `fsync()` it, then `rename()` it
   over `name`. If the power is cut at any moment, the old file or the new
   one survives intact. Players switch pinball machines off at the wall.
2. **Validate on read.** ALP Snake's files carry a magic number and a check
   value:
   - the high-score table has a checksum, and also a check that the scores
     are in order;
   - `settings.dat` stores the bits *and* their inverse.

   Anything that fails is ignored and defaults are used.
3. **Migrate old formats.** Each layout has its own magic (`SNKH`, `SNK2`,
   `SNK3`), so a newer game can read an older file and convert it instead of
   losing a player's record.
4. **Save at the right moments.** Save as soon as initials are entered, and
   also in `retro_unload_game`. Quitting from the cabinet menu during a game
   still keeps a qualifying score, filed under the last-used initials.

**The top-ten table.** It is `hiscore.dat`: 10 × (score, 3 initials), best
first. A blank table is filled with sample scores (PFI 600, SNK 500, HOT 300,
KLR 150) so the attract screen looks lived-in.

---

## 11. Settings (DIP switches)

Real arcade boards have DIP switches. Players expect them under **Adv.
Config**. ALP Snake's are Hawk, Mouse, Slow signs, Move sound and Attract
music. The flow:

1. The player opens the cabinet menu and chooses **Adv. Config**.
2. On the next frame, `GET_VARIABLE_UPDATE` reports a change and
   `mame2003-plus_display_setup` reads `"enabled"`. The core pauses the game
   and draws its own settings page.
3. Any button flips the highlighted switch, or leaves the page on EXIT.
   Changes take effect immediately and are saved to `save/settings.dat`.
4. When the cabinet menu closes, the variable returns to `"disabled"` and the
   page closes. The `retro_reset()` that comes with it is ignored.

Why not libretro core options? The cabinet doesn't display them, and it
returns defaults for unknown variables, so they can't be stored there.

---

## 12. Diagnostics on the real machine

### The flipper overlay

Hold **both flippers** on the attract, Ready or Game Over screen:

```
FPS 5994  SND 100  GAP 041  DRAW 023
```

| Field | Meaning |
|---|---|
| FPS | real frames per second × 100 |
| SND | % of offered audio Retroplayer accepted |
| GAP | worst gap between two frames in the last ~2 s, in ms |
| DRAW | worst full-redraw time, in ms |

This is how the choppy-audio and jitter problems were understood.

### The probe build

Run `ALP_SNAKE_PROBE=1 ./build.sh`. It builds `ALP-Snake-Probe.UCE`, which logs
these to `save/probe.log`:
- every libretro call;
- environment request;
- variable change;
- button and key press;
- frame timing.

Play it on the cabinet, copy the `.UCE` back, and read the log with:

```sh
python3 tools/inspect_uce.py dist/ALP-Snake-Probe.UCE --extract probe-out/
```

The probe's `exec.sh` also tries to copy Retroplayer's output to the USB
stick. That didn't work, because we never found where the cabinet mounts the
stick (no `/media/usbN`). The save-area route always works.

### inspect_uce.py

`tools/inspect_uce.py` checks the UCE layout and MD5s, and lists the save
folder. It decodes the high-score table and the settings, and with
`--extract` copies the save files out.

---

## 13. Testing without the cabinet

Run everything with:

```sh
tests/run_tests.sh
```

It works on macOS or Linux with any C compiler (`cc`). Every test is built
with `-fsanitize=address,undefined`, which catches memory errors the cabinet
would only show as a crash.

Keep the rules hardware-free. `snake_game.c` needs only `<stdint.h>`, so it
compiles anywhere. That made these possible:

| Test | What it does |
|---|---|
| `tests/game_fuzz.c` | 20,000 games with a half-sensible, half-random player. After every move it checks the body is connected and never overlaps itself, and that length and score match an independent model of the rules |
| `tests/hawk_test.c` | 5,000 hawk attacks. It checks the hawk circles before it dives, catches only inside its 3×3 zone at the right moment, and that turning at once always escapes |
| `tests/sign_test.c` | Long autopiloted games. It checks SLOW signs (never at the starting speed, never on food or the snake, gone within 70 moves, spread over the whole board) and the speed-level rules |
| `tests/frontend_test.c` | The whole core inside a fake Retroplayer; see below |

**The fake Retroplayer.** `frontend_test.c` `#include`s `snake_libretro.c`
and is built with `-DALP_SNAKE_HOST_PREVIEW`:

- **No real system calls.** In this mode `system_call()` returns -1, so no
  files or devices are touched.
- **Repeatable.** With no clock, timing falls back to counting frames, so
  every run is the same.
- **What it plays.** The test answers environment calls the way the cabinet
  does, and replays the cabinet's Adv. Config sequence (the variable plus the
  R3 tap). It runs attract mode, Insert Coin, the DIP page, a game over, and
  the return to attract.
- **Audio.** It counts the audio the core produces.
- **Screenshots.** It saves PPM images to `tests/out/`; the README's images
  come from here.

**Real system calls.** The same kind of harness can also be built *without*
`ALP_SNAKE_HOST_PREVIEW` on an AArch64 Linux machine (a VM is fine). Then
saving, settings and descriptor handling run against a real kernel. That is
how the high-score and settings files were tested during development.

Every change should still be checked on the cabinet. Display, SDL and timing
behaviour can only be verified there.

---

## 14. Pitfalls we hit

- **Frame-based timing:** the snake slowed when the cabinet was busy. Use the
  clock ([§6](#6-timing)).
- **Fixed 735 samples per frame:** this drifted and made sound choppy in long
  games. Push samples by elapsed time and never drop any ([§7](#7-sound)).
- **Music driven by frames:** this sounded jittery. Count the tempo in
  samples.
- **`= {0}` on a big array:** an undefined `memset` broke the link. Use
  `static`.
- **Measuring text before the font loaded:** the title was off-centre on the
  first frame.
- **`int16_t` colour arithmetic:** values overflowed. Use 32-bit
  intermediates.
- **Waiting for Start only on a menu:** players press whatever button is
  nearest. Accept any button.
- **Weak random numbers:** a basic LCG's lowest bit alternates 0, 1, 0, 1. So
  two `% even_number` draws in a row always had opposite parity, and SLOW
  signs could only appear on half the squares. Use the high bits.
- **x86 flag values:** `O_DIRECTORY` differs on arm64 ([§3](#3-building-without-a-c-library)).
- **Save Slots and Display Mode → Fill:** they look supported but don't
  persist. Don't build on them.
- **Replacing the UCE resets the saves:** back up the played file first.
- **Finder-launched `.command` scripts:** they don't get your shell's `PATH`.
  Start them with `#!/bin/bash` and add Homebrew's paths yourself.
- **Unknown USB mount point:** exec.sh can't reliably write to the stick. Read
  results from the save area instead.

---

## 15. Checklist for a new game

1. **Start from this repo.** Replace `snake_game.*` with your rules, keeping
   them hardware-free so you can fuzz them.
2. Keep the MAME 2003-Plus identity if you want Insert Coin and Adv. Config.
   If you don't, choose your own core name and get the smaller menu.
3. Draw into the 1080 × 1920 RGB565 buffers. Set `frame_dirty` only when
   something visible changes.
4. Drive all game timing from `CLOCK_MONOTONIC`, with the nearest-frame and
   resume-gap rules.
5. Generate audio from the clock (`audio_frames_due`). Never drop samples, and
   keep the mix under full scale.
6. **Save** with temp file, fsync and rename; use magic numbers; validate
   everything on load.
7. **Backglass:** change `snake_backglass_state.h` to carry what you want to
   show. Rebuild both programs together and bump the magic.
8. **Before every cabinet test:** build with `-Wall -Wextra` and check for
   undefined symbols. Run your fuzzers and the host harnesses, and confirm
   the diagnostics overlay shows FPS ≈ 5994, SND 100 and a reasonable DRAW.
9. **Cabinet behaviour:** build the probe once to confirm Retroplayer treats
   your core the way this guide describes on your firmware.
10. **Licences and content:** ship only your own code, art and sound, and fonts
    whose licences allow it. No ROMs, no firmware, no AtGames artwork.

---

## 16. Source map

| File | What to learn from it |
|---|---|
| `src/snake_game.h/.c` | A deterministic, hardware-free rules engine; the demo autopilot (flood fill) |
| `src/snake_libretro.c` | The libretro core: lifecycle, the MAME identity, drawing, timing, audio clock, trackball, saving, DIP page, attract mode, backglass launch, diagnostics |
| `src/snake_audio.h/.c` | A software synth without libm, sound-effect recipes, a sample-accurate music sequencer |
| `src/snake_backglass.c` | A no-libc SDL2 program on the second screen |
| `src/snake_backglass_state.h` | The core ↔ helper mailbox format |
| `src/snake_font.h`, `src/snake_smooth_font.*` | Bitmap font and anti-aliased font atlas |
| `src/alp_syscall.h` | arm64 system call numbers and flags |
| `tools/build.py` | Compiler flags explained; recipe layout; exec.sh |
| `tools/package_uce.py` | UCE byte layout; the ext4 save image |
| `tools/inspect_uce.py` | Checking a UCE and reading its saves |
| `tools/sdl2_link_stub.c` | Linking against SDL2 without SDL2 |
| `tools/make_*.swift` | Generating box art, backglass art and the font atlas on macOS |
| `tests/` | Fuzzers, scenario tests and a fake Retroplayer (§13) |

---

*ALP Snake and this guide are an independent fan project, not affiliated with,
endorsed by or supported by AtGames Digital Media, Inc. AtGames, Legends
Pinball and the other AtGames names are trademarks of AtGames; all other
trademarks belong to their respective owners. See the README's
[Trademarks and disclaimer](README.md#trademarks-and-disclaimer).*
