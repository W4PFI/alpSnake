/*
 * snake_font.h - a tiny 5x7 pixel bitmap font, plus number formatting.
 *
 * This is the fallback / "retro" font. The big, smooth headings use the
 * pre-rendered Nunito atlas (snake_smooth_font.c); this blocky font is used
 * for small text and whenever that atlas fails to load. It is included by
 * both the core (snake_libretro.c) and the backglass helper
 * (snake_backglass.c).
 *
 * How the glyphs are stored: each character is 7 bytes, one per row from top
 * to bottom. Only the low 5 bits of each byte are used, and bit 4 (value 16)
 * is the LEFTMOST pixel, bit 0 (value 1) the rightmost. For example 'A' is
 *      14 = .###.
 *      17 = #...#
 *      17 = #...#
 *      31 = #####
 *      17 = #...#   (and two more rows of 17)
 * A renderer loops over 7 rows and 5 columns and tests
 * `row_bits & (1u << (4 - column))`, drawing each set bit as a scale x scale
 * square, with one blank column between characters (6 columns per letter).
 *
 * Only A-Z, 0-9, ':', '-' and '/' exist; anything else (including lower case
 * and space) draws as blank. Everything is `static` in a header so each file
 * that includes it gets its own private copy and no separate .c is needed;
 * the tables are tiny. Keep in mind that the smooth font has a different,
 * smaller set of characters (no ':', '-' or '/').
 */
#ifndef ALP_SNAKE_FONT_H
#define ALP_SNAKE_FONT_H

#include <stdint.h>

/* 'A'..'Z', 7 rows each (see the bit layout above). */
static const uint8_t snake_letters[26][7] = {
   {14,17,17,31,17,17,17}, {30,17,17,30,17,17,30},
   {14,17,16,16,16,17,14}, {30,17,17,17,17,17,30},
   {31,16,16,30,16,16,31}, {31,16,16,30,16,16,16},
   {14,17,16,23,17,17,15}, {17,17,17,31,17,17,17},
   {31,4,4,4,4,4,31}, {7,2,2,2,18,18,12},
   {17,18,20,24,20,18,17}, {16,16,16,16,16,16,31},
   {17,27,21,21,17,17,17}, {17,25,21,19,17,17,17},
   {14,17,17,17,17,17,14}, {30,17,17,30,16,16,16},
   {14,17,17,17,21,18,13}, {30,17,17,30,20,18,17},
   {15,16,16,14,1,1,30}, {31,4,4,4,4,4,4},
   {17,17,17,17,17,17,14}, {17,17,17,17,17,10,4},
   {17,17,17,21,21,21,10}, {17,17,10,4,10,17,17},
   {17,17,10,4,4,4,4}, {31,1,2,4,8,16,31}
};

/* '0'..'9'. The zero has a diagonal slash-like stroke (rows 19,21,25) to
   tell it apart from the letter O. */
static const uint8_t snake_digits[10][7] = {
   {14,17,19,21,25,17,14}, {4,12,4,4,4,4,14},
   {14,17,1,2,4,8,31}, {30,1,1,14,1,1,30},
   {2,6,10,18,31,2,2}, {31,16,30,1,1,17,14},
   {6,8,16,30,17,17,14}, {31,1,2,4,8,8,8},
   {14,17,17,14,17,17,14}, {14,17,17,15,1,2,12}
};

/* The few punctuation marks the screens need, and an all-blank glyph for
   anything unknown. */
static const uint8_t snake_colon[7] = {0,4,4,0,4,4,0};
static const uint8_t snake_dash[7] = {0,0,0,31,0,0,0};
static const uint8_t snake_slash[7] = {1,1,2,4,8,16,16};
static const uint8_t snake_blank[7] = {0,0,0,0,0,0,0};

/* Returns the 7-row bitmap for a character; unknown characters (including
   space and lower case) give the blank glyph, never NULL, so callers can
   draw any string without checking. */
static const uint8_t *snake_glyph(char character)
{
   if (character >= 'A' && character <= 'Z')
      return snake_letters[character - 'A'];
   if (character >= '0' && character <= '9')
      return snake_digits[character - '0'];
   if (character == ':')
      return snake_colon;
   if (character == '-')
      return snake_dash;
   if (character == '/')
      return snake_slash;
   return snake_blank;
}

/* Writes `value` as exactly `digits` decimal digits, zero-padded on the left
   and filled from the right ("007"). Higher digits that do not fit are
   silently dropped. It does NOT add a terminating 0 byte - the caller places
   the digits inside a larger string. (Hand-written because there is no
   sprintf in the freestanding build.) */
static void snake_decimal(char *destination, unsigned value, unsigned digits)
{
   while (digits)
   {
      destination[--digits] = (char)('0' + value % 10);
      value /= 10;
   }
}

/* Scores show as four zero-padded digits ("0120"), growing to five once
   they pass 9999 (and clamped at 99999). The result is 0-terminated, so
   destination needs room for six characters. */
static void snake_score_text(char *destination, unsigned value)
{
   unsigned digits = value > 9999 ? 5 : 4;
   if (value > 99999)
      value = 99999;
   snake_decimal(destination, value, digits);
   destination[digits] = 0;
}

#endif
