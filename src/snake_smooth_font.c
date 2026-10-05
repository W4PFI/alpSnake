/*
 * snake_smooth_font.c - loads the pre-rendered Nunito font atlas
 * (emu/snake-font.bin) and answers questions about its glyphs.
 *
 * Role: a small, self-contained loader shared by the libretro core and the
 * backglass helper (each links its own copy). Drawing is done by the
 * callers: the core scales and alpha-blends glyphs into its RGB565
 * framebuffer, the backglass helper turns each glyph into an SDL texture.
 *
 * Key ideas for a learner:
 *  - Pre-rendering a font to raw alpha bitmaps on a desktop machine lets a
 *    freestanding program show nice anti-aliased text with no FreeType, no
 *    libc and no file-format parsing: it is just one read() into an array.
 *  - The file is validated (magic "SNK1" and exact size) before use, and any
 *    failure is remembered so the caller quietly uses the 5x7 bitmap font.
 *  - Glyphs are drawn centred in a 128-pixel-wide box, so after loading we
 *    scan each one for the columns that contain ink. That gives proportional
 *    spacing ("I" narrow, "W" wide) without storing any metrics in the file.
 *  - The atlas holds A-Z, 0-9 and space only (37 glyphs). There is no '-',
 *    ':' or '/': such characters get no glyph from snake_smooth_glyph().
 *  - With ALP_SNAKE_HOST_PREVIEW defined (a desktop build that renders
 *    preview screenshots) the file is read with stdio from
 *    assets/snake-font.bin, relative to the project folder, instead of via
 *    raw system calls.
 */
#include "alp_syscall.h"
#include "snake_smooth_font.h"

#include <stddef.h>

#ifdef ALP_SNAKE_HOST_PREVIEW
#include <stdio.h>
#endif

/* 26 letters + 10 digits + space, and the total glyph bytes after the
   4-byte header: 37 * 128 * 144 = 681,984 bytes. */
#define GLYPH_COUNT 37
#define FONT_DATA_SIZE (GLYPH_COUNT * SNAKE_FONT_WIDTH * SNAKE_FONT_HEIGHT)

/* The whole atlas lives in static (zero-initialised, .bss) memory: about
   670 KB, far too big for the stack, and no malloc exists in this build. */
static uint8_t font_data[FONT_DATA_SIZE];
/* Per glyph: first inked column and inked width, filled in by the load. */
static uint8_t glyph_left[GLYPH_COUNT];
static uint8_t glyph_width[GLYPH_COUNT];
/* 0 = not tried yet, 1 = loaded and valid, -1 = failed (do not retry). */
static int font_loaded;

/* Maps a character to its position in the atlas, matching the order in
   tools/make_smooth_font.swift ("A..Z0..9 "), or -1 if it has no glyph
   (punctuation, lower case, anything else). */
static int glyph_index(char letter)
{
   return letter >= 'A' && letter <= 'Z' ? letter - 'A' :
      letter >= '0' && letter <= '9' ? letter - '0' + 26 :
      letter == ' ' ? 36 : -1;
}

#ifndef ALP_SNAKE_HOST_PREVIEW
/* A raw Linux system call on aarch64: the call number goes in register x8,
   arguments in x0..x3, and "svc #0" traps into the kernel; the result comes
   back in x0 (negative errno on failure). This file has its own static copy
   of the helper (the core and the backglass helper each have one too) so it
   stays self-contained with no libc. Only the real cabinet build uses it. */
static long font_call(long number, long first, long second, long third, long fourth)
{
   register long arg0 asm("x0") = first;
   register long arg1 asm("x1") = second;
   register long arg2 asm("x2") = third;
   register long arg3 asm("x3") = fourth;
   register long call_number asm("x8") = number;
   asm volatile("svc #0" : "+r"(arg0) : "r"(arg1), "r"(arg2),
                "r"(arg3), "r"(call_number) : "memory");
   return arg0;
}
#endif

/* Reads and checks the atlas, then measures each glyph. Called lazily the
   first time text is drawn; the result is cached in font_loaded, so a
   missing file costs one failed open() and not one per frame. */
int snake_smooth_font_load(void)
{
   if (font_loaded)
      return font_loaded > 0;
   /* Assume failure until the file has been fully checked. */
   font_loaded = -1;
   uint8_t header[4];
#ifdef ALP_SNAKE_HOST_PREVIEW
   FILE *source = fopen("assets/snake-font.bin", "rb");
   if (!source)
      return 0;
   size_t header_bytes = fread(header, 1, sizeof(header), source);
   size_t glyph_bytes = fread(font_data, 1, sizeof(font_data), source);
   fclose(source);
#else
   /* Relative path: the cabinet runs exec.sh (which itself uses ./emu/...
      paths) from the mounted UCE folder, and the core and backglass helper
      inherit that working directory, so the atlas is at ./emu/. */
   long descriptor = font_call(SYS_openat, AT_FDCWD, (long)"./emu/snake-font.bin", O_RDONLY, 0);
   if (descriptor < 0)
      return 0;
   long header_bytes = font_call(SYS_read, descriptor, (long)header, sizeof(header), 0);
   size_t glyph_bytes = 0;
   /* read() may return less than asked for, so keep reading until the whole
      atlas is in (or the file ends early, which fails the size check). */
   while (glyph_bytes < sizeof(font_data))
   {
      long count = font_call(SYS_read, descriptor, (long)(font_data + glyph_bytes),
                             sizeof(font_data) - glyph_bytes, 0);
      if (count <= 0)
         break;
      glyph_bytes += (size_t)count;
   }
   font_call(SYS_close, descriptor, 0, 0, 0);
#endif
   int valid = header_bytes == sizeof(header) && header[0] == 'S' &&
      header[1] == 'N' && header[2] == 'K' && header[3] == '1' &&
      glyph_bytes == sizeof(font_data);
   /* Find each glyph's inked columns. Alpha above 16 (about 6%) counts as
      ink, so the faint anti-aliasing fringe does not widen the spacing.
      An empty glyph (space) gets left 0, width 0. */
   if (valid)
      for (int index = 0; index < GLYPH_COUNT; ++index)
      {
         const uint8_t *glyph = font_data + index * SNAKE_FONT_WIDTH * SNAKE_FONT_HEIGHT;
         int first = SNAKE_FONT_WIDTH;
         int last = -1;
         for (int column = 0; column < SNAKE_FONT_WIDTH; ++column)
            for (int row = 0; row < SNAKE_FONT_HEIGHT; ++row)
               if (glyph[row * SNAKE_FONT_WIDTH + column] > 16)
               {
                  if (column < first)
                     first = column;
                  if (column > last)
                     last = column;
               }
         glyph_left[index] = last < 0 ? 0 : (uint8_t)first;
         glyph_width[index] = last < 0 ? 0 : (uint8_t)(last - first + 1);
      }
   font_loaded = valid ? 1 : -1;
   return valid;
}

/* Returns the glyph's alpha image (row-major, SNAKE_FONT_WIDTH bytes per
   row) or NULL when the font is not loaded or the character is missing. */
const uint8_t *snake_smooth_glyph(char letter)
{
   if (font_loaded <= 0)
      return 0;
   int index = glyph_index(letter);
   return index < 0 ? 0 : font_data + index * SNAKE_FONT_WIDTH * SNAKE_FONT_HEIGHT;
}

/* Returns the inked column range measured at load time; 0, 0 for space and
   for characters not in the atlas. */
void snake_smooth_glyph_bounds(char letter, int *left, int *width)
{
   int index = glyph_index(letter);
   *left = index < 0 ? 0 : glyph_left[index];
   *width = index < 0 ? 0 : glyph_width[index];
}

/* Pen advance after a character, in screen pixels. Callers scale glyphs so
   that 96 atlas pixels become `cell_width` screen pixels horizontally, so
   the advance is the scaled inked width (the + 48 rounds to nearest) plus a
   gap of cell_width / 12. A blank (space, or a character with no glyph)
   advances half a cell. Note that text drawers which skip missing glyphs
   without advancing will disagree with this when measuring such strings. */
int snake_smooth_advance(char letter, int cell_width)
{
   int left;
   int width;
   snake_smooth_glyph_bounds(letter, &left, &width);
   return width ? (width * cell_width + 48) / 96 + cell_width / 12 :
      cell_width / 2;
}
