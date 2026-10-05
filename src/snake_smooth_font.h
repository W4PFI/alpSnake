/*
 * snake_smooth_font.h - the smooth, anti-aliased Nunito font used for big
 * headings and scores, on both the playfield and the backglass.
 *
 * There is no font engine on the cabinet we can use from a no-libc core, so
 * the letters are rendered ahead of time on a Mac by
 * tools/make_smooth_font.swift (Nunito ExtraBold at 112 pt) into
 * assets/snake-font.bin, which the build copies into the UCE as
 * emu/snake-font.bin. At run time the game just loads that file and scales /
 * blends the glyph images itself. If the file is missing or damaged, callers
 * fall back to the blocky 5x7 font in snake_font.h.
 *
 * File format: the 4 bytes "SNK1", then 37 glyphs in the order
 * "A".."Z", "0".."9", " ", each SNAKE_FONT_WIDTH x SNAKE_FONT_HEIGHT bytes of
 * 8-bit alpha (0 = transparent, 255 = solid), row by row from the top.
 * There are NO punctuation glyphs: ':', '-', '/' and lower case are not in
 * the atlas, so text drawn in this font must stick to A-Z, 0-9 and space.
 */
#ifndef SNAKE_SMOOTH_FONT_H
#define SNAKE_SMOOTH_FONT_H

#include <stdint.h>

/* Size of one glyph image in the atlas, in pixels. */
#define SNAKE_FONT_WIDTH 128
#define SNAKE_FONT_HEIGHT 144

/* Loads and validates the atlas the first time it is called (later calls
   return the cached answer, including a cached failure). Returns 1 if the
   smooth font is usable, 0 if callers should use the bitmap font instead. */
int snake_smooth_font_load(void);
/* Returns the SNAKE_FONT_WIDTH x SNAKE_FONT_HEIGHT alpha image for a
   letter, or NULL if the font is not loaded or has no such character. */
const uint8_t *snake_smooth_glyph(char letter);
/* Reports which columns of the glyph image actually contain ink (the first
   column and how many), so text can be drawn with tight, proportional
   spacing instead of fixed 128-pixel cells. Unknown characters and space
   report 0, 0. Valid only after a successful snake_smooth_font_load(). */
void snake_smooth_glyph_bounds(char letter, int *left, int *width);
/* How far, in screen pixels, the pen moves after drawing `letter` when text
   is drawn at size `cell_width` (see the .c file for the scale). Used both
   to draw and to measure strings for centring. */
int snake_smooth_advance(char letter, int cell_width);

#endif
