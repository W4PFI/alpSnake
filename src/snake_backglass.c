/*
 * snake_backglass.c - the backglass helper program, alp_snake_backglass.
 *
 * ROLE IN THE SYSTEM
 * The cabinet has a 1080x1920 portrait playfield screen and a 1920x1080
 * landscape backglass screen above it. Retroplayer only gives a libretro core
 * the playfield, so the core (snake_libretro.c) starts THIS separate program
 * to own the backglass. It is a stand-alone Linux executable, not a library:
 *
 *   core  --clone()+execve()-->  ./emu/alp_snake_backglass
 *         (env ForceConnectID=93 makes the cabinet's SDL2 open the backglass
 *          display; PR_SET_PDEATHSIG kills us if the core dies)
 *   core  --writes-->  /tmp/alp-snake-state.bin  --polled by-->  this program
 *
 * The file format is struct backglass_state in snake_backglass_state.h. We
 * show the artwork (boxart/snake-backglass.bmp, made by
 * tools/make_backglass_art.swift), a score panel with SCORE and BEST, a
 * "NEW HIGH SCORE"/"TOP TEN SCORE" banner while initials are entered, and
 * confetti for a new best score. When the core deletes the state file on
 * unload, we clean up and exit.
 *
 * KEY IDEAS FOR A LEARNER
 * - No C library. Like the core, this is built with -nostdlib: there is no
 *   main(), no printf, no exit(). Execution begins at _start() and we talk
 *   to it with raw "svc #0" system calls (system_call() below).
 * - SDL2 without SDL headers. We declare the dozen SDL functions we need by
 *   hand with void* handles and magic numbers for flags/enums (each one is
 *   explained where used). At build time we link against a stub library
 *   (tools/sdl2_link_stub.c); at run time the real libSDL2-2.0.so.0 on the
 *   cabinet is loaded by the system's dynamic linker. That is why, unlike the
 *   core, this program is linked with --dynamic-linker (see tools/build.py).
 * - No memset/memcpy. The compiler may turn "= {0}" on big objects or struct
 *   copies into memset/memcpy calls, which don't exist here. So large or
 *   zero-initialised data is declared static (zeroed by the loader) and copies
 *   are written as loops.
 * - Cheap polling. Redraw only when something visible changed, plus a slow
 *   periodic refresh and while confetti is moving; otherwise just sleep.
 *
 * Coordinates are SDL's: origin at the top-left, y grows downwards.
 */
#include "alp_syscall.h"
#include "snake_backglass_state.h"
#include "snake_font.h"
#include "snake_game.h"
#include "snake_smooth_font.h"

#include <stdbool.h>
#include <stdint.h>

/* system_call() below uses AArch64 registers and syscall numbers. */
#if !defined(__aarch64__)
#error This helper targets AArch64 Linux
#endif

/* Same layout as SDL_Rect, so a pointer to it can be passed to SDL. */
struct rectangle {
   int x;
   int y;
   int width;
   int height;
};

/* Hand-written declarations of the SDL2 functions we use. Real SDL types
   (SDL_Window*, SDL_Renderer*, SDL_Texture*, SDL_Surface*, SDL_RWops*) are all
   just pointers, so void* is ABI-compatible; enums and flags are plain ints. */
extern int SDL_Init(unsigned flags);
extern void SDL_Quit(void);
extern void *SDL_CreateWindow(const char *title, int x, int y, int width, int height, unsigned flags);
extern void SDL_DestroyWindow(void *window);
extern void *SDL_CreateRenderer(void *window, int index, unsigned flags);
extern void SDL_DestroyRenderer(void *renderer);
extern void *SDL_RWFromFile(const char *file, const char *mode);
extern void *SDL_LoadBMP_RW(void *source, int free_source);
extern void SDL_FreeSurface(void *surface);
extern void *SDL_CreateTextureFromSurface(void *renderer, void *surface);
extern void *SDL_CreateTexture(void *renderer, unsigned format, int access, int width, int height);
extern void SDL_DestroyTexture(void *texture);
extern int SDL_UpdateTexture(void *texture, const void *rectangle, const void *pixels, int pitch);
extern int SDL_SetTextureBlendMode(void *texture, int blend_mode);
extern int SDL_SetTextureColorMod(void *texture, uint8_t red, uint8_t green, uint8_t blue);
extern int SDL_RenderCopy(void *renderer, void *texture, const void *source, const void *destination);
extern int SDL_SetRenderDrawColor(void *renderer, uint8_t red, uint8_t green, uint8_t blue, uint8_t alpha);
extern int SDL_RenderFillRect(void *renderer, const struct rectangle *rectangle);
extern void SDL_RenderPresent(void *renderer);
extern void SDL_Delay(unsigned milliseconds);

/* Raw Linux system call with up to three arguments (AArch64 convention:
   number in x8, arguments in x0..x2, result in x0; negative = -errno).
   The "memory" clobber tells the compiler the kernel may read or write
   memory we passed pointers to, so it must not cache values across the call. */
static long system_call(long number, long first, long second, long third)
{
   register long arg0 asm("x0") = first;
   register long arg1 asm("x1") = second;
   register long arg2 asm("x2") = third;
   register long call_number asm("x8") = number;
   asm volatile("svc #0" : "+r"(arg0) : "r"(arg1), "r"(arg2), "r"(call_number) : "memory");
   return arg0;
}

/* Writes a C string to standard output (fd 1). Handy as a log line when the
   helper is run by hand; there is no printf without libc. */
static void print_text(const char *message)
{
   unsigned length = 0;
   while (message[length])
      ++length;
   system_call(SYS_write, 1, (long)message, length);
}

/* Reads the core's state file into *state.
   Returns -1 if the file does not exist (the core has unloaded: time to quit),
   1 if a complete, valid state was read, and 0 if the file exists but is
   short or has the wrong magic (keep running, just don't draw it). Because
   the core replaces the file with an atomic rename, a read normally sees
   either the whole old snapshot or the whole new one. */
static int read_state(struct backglass_state *state)
{
   long descriptor = system_call(SYS_openat, AT_FDCWD, (long)SNAKE_STATE_PATH, O_RDONLY | O_CLOEXEC);
   if (descriptor < 0)
      return -1;
   long bytes = system_call(SYS_read, descriptor, (long)state, sizeof(*state));
   system_call(SYS_close, descriptor, 0, 0);
   return bytes == sizeof(*state) && state->magic == SNAKE_STATE_MAGIC;
}

/* Sets the opaque colour used by the next fill(). */
static void color(void *renderer, uint8_t red, uint8_t green, uint8_t blue)
{
   SDL_SetRenderDrawColor(renderer, red, green, blue, 255);
}

/* Fills a rectangle with the current draw colour. */
static void fill(void *renderer, int x, int y, int width, int height)
{
   struct rectangle area = {x, y, width, height};
   SDL_RenderFillRect(renderer, &area);
}

/* Fallback text renderer: draws the built-in 5x7 pixel font (snake_font.h),
   one filled square per lit pixel, `scale` screen pixels per font pixel.
   Each character advances 6 font pixels (5 wide + 1 gap). Each glyph row is a
   byte whose low 5 bits are the columns, bit 4 = leftmost. Only used when the
   smooth font atlas could not be loaded. Uses the current draw colour. */
static void draw_text(void *renderer, const char *message, int x, int y, int scale)
{
   for (unsigned letter = 0; message[letter]; ++letter)
   {
      const uint8_t *glyph = snake_glyph(message[letter]);
      for (int row = 0; row < 7; ++row)
         for (int column = 0; column < 5; ++column)
            if (glyph[row] & (1u << (4 - column)))
               fill(renderer, x + (int)letter * 6 * scale + column * scale,
                    y + row * scale, scale, scale);
   }
}

/* Maps 'A'-'Z' to 0-25 and '0'-'9' to 26-35 (the order of font_textures[]);
   anything else, such as a space, returns -1 (no texture). */
static int glyph_index(char letter)
{
   return letter >= 'A' && letter <= 'Z' ? letter - 'A' :
      letter >= '0' && letter <= '9' ? letter - '0' + 26 : -1;
}

/* Turns the anti-aliased font atlas (emu/snake-font.bin, an 8-bit coverage
   mask per glyph; see snake_smooth_font.c) into one SDL texture per letter
   and digit. Each texture is white with the glyph's coverage as alpha, so
   later we can tint it any colour with SDL_SetTextureColorMod and let SDL
   alpha-blend the smooth edges. Leaves entries NULL on failure; callers then
   fall back to the pixel font. */
static void make_font_textures(void *renderer, void **textures)
{
   if (!snake_smooth_font_load())
      return;
   /* 128x144x4 bytes is too big for comfort on the stack and must not be
      zeroed with "= {0}" (memset); static avoids both. Reused per glyph. */
   static uint32_t pixels[SNAKE_FONT_WIDTH * SNAKE_FONT_HEIGHT];
   for (int index = 0; index < 36; ++index)
   {
      char letter = index < 26 ? (char)('A' + index) : (char)('0' + index - 26);
      const uint8_t *glyph = snake_smooth_glyph(letter);
      for (int pixel = 0; pixel < SNAKE_FONT_WIDTH * SNAKE_FONT_HEIGHT; ++pixel)
         pixels[pixel] = 0xffffff00u | glyph[pixel]; /* R=G=B=255, A=coverage */
      /* 0x16462004 = SDL_PIXELFORMAT_RGBA8888 (a 32-bit value laid out
         0xRRGGBBAA, matching the line above); access 0 = STATIC (uploaded
         once). Blend mode 1 = SDL_BLENDMODE_BLEND (normal alpha blending). */
      void *texture = SDL_CreateTexture(renderer, 0x16462004u, 0,
                                        SNAKE_FONT_WIDTH, SNAKE_FONT_HEIGHT);
      if (texture && SDL_UpdateTexture(texture, 0, pixels, SNAKE_FONT_WIDTH * 4) == 0 &&
          SDL_SetTextureBlendMode(texture, 1) == 0)
         textures[index] = texture;
      else if (texture)
         SDL_DestroyTexture(texture);
   }
}

/* Draws a string with the smooth font, left edge at x, top at y, tinted
   (red, green, blue). cell_width and cell_height set the size: atlas pixels
   are scaled horizontally by cell_width/96 (the "+ 48) / 96" rounds to the
   nearest pixel; the same scale as snake_smooth_advance) and the full
   144-pixel atlas height (which includes space above and below the letter)
   is scaled to cell_height; the two scales need not match. Only each
   glyph's inked columns are copied (snake_smooth_glyph_bounds), and the cursor moves by the glyph's
   proportional advance, so the text is proportionally spaced. Characters
   without a texture (space) just advance. If the atlas failed to load
   (textures[0] is NULL) it falls back to the blocky pixel font. */
static void draw_smooth_text(void *renderer, void **textures, const char *message,
                             int x, int y, int cell_width, int cell_height,
                             uint8_t red, uint8_t green, uint8_t blue)
{
   if (!textures[0])
   {
      color(renderer, red, green, blue);
      draw_text(renderer, message, x, y, cell_height / 9); /* 7 rows + margin */
      return;
   }
   int cursor = x;
   for (int letter = 0; message[letter]; ++letter)
   {
      int index = glyph_index(message[letter]);
      if (index < 0 || !textures[index])
      {
         cursor += snake_smooth_advance(message[letter], cell_width);
         continue;
      }
      int source_left;
      int source_width;
      snake_smooth_glyph_bounds(message[letter], &source_left, &source_width);
      struct rectangle source = {source_left, 0, source_width, SNAKE_FONT_HEIGHT};
      SDL_SetTextureColorMod(textures[index], red, green, blue);
      struct rectangle destination = {cursor, y,
                                      (source_width * cell_width + 48) / 96, cell_height};
      SDL_RenderCopy(renderer, textures[index], &source, &destination);
      cursor += snake_smooth_advance(message[letter], cell_width);
   }
}

/* Width in pixels that draw_smooth_text() would use for message, so text can
   be centred: x = centre - text_width(...) / 2. */
static int text_width(const char *message, int cell_width)
{
   int width = 0;
   for (int letter = 0; message[letter]; ++letter)
      width += snake_smooth_advance(message[letter], cell_width);
   return width;
}

/* Confetti drizzling down either side of the mascot during a new high
   score. Positions and velocities are fixed-point in 1/16 pixel (no floating
   point needed); the main loop ticks about 30 times a second (SDL_Delay(33)).
   Pieces live in a fixed pool of DRIZZLE_COUNT slots: spawning reuses a dead
   slot, and when the pool is full new pieces are simply skipped. */
#define DRIZZLE_COUNT 120
#define DRIZZLE_FLOOR 735 /* top edge of the score panel */
struct drizzle {
   int x;
   int y;
   int vx;
   int vy;
   uint8_t red;
   uint8_t green;
   uint8_t blue;
   uint8_t width;
   uint8_t height;
   uint8_t flip;
   bool live;
};
static struct drizzle drizzle[DRIZZLE_COUNT];
static uint32_t drizzle_random = 0x9e3779b9u; /* any non-zero seed works */
static unsigned drizzle_tick; /* counts drizzle_update() calls; drives animation */

/* Xorshift32 pseudo-random generator (shifts 13, 17, 5): tiny, no libc,
   plenty random enough for confetti. */
static uint32_t drizzle_next(void)
{
   drizzle_random ^= drizzle_random << 13;
   drizzle_random ^= drizzle_random >> 17;
   drizzle_random ^= drizzle_random << 5;
   return drizzle_random;
}

/* Starts one confetti piece just above the top of the screen at a random x in
   [left, right) pixels, with a random colour, size, sideways drift
   (-16..16 sixteenths/tick), fall speed (80..149 sixteenths/tick, i.e. about
   5-9 px per tick) and tumble phase. Does nothing if all slots are in use. */
static void drizzle_spawn(int left, int right)
{
   static const uint8_t colors[8][3] = {
      {255, 40, 40}, {255, 220, 40}, {0, 220, 255}, {255, 60, 220},
      {60, 238, 144}, {255, 140, 0}, {255, 255, 255}, {150, 230, 60}
   };
   for (int index = 0; index < DRIZZLE_COUNT; ++index)
   {
      struct drizzle *piece = &drizzle[index];
      if (piece->live)
         continue;
      const uint8_t *color = colors[drizzle_next() % 8];
      piece->live = true;
      piece->x = (left + (int)(drizzle_next() % (uint32_t)(right - left))) * 16;
      piece->y = -20 * 16;
      piece->vx = (int)(drizzle_next() % 33) - 16;
      piece->vy = 80 + (int)(drizzle_next() % 70);
      piece->red = color[0];
      piece->green = color[1];
      piece->blue = color[2];
      piece->width = (uint8_t)(14 + drizzle_next() % 10);
      piece->height = (uint8_t)(8 + drizzle_next() % 6);
      piece->flip = (uint8_t)(drizzle_next() % 16);
      return;
   }
}

/* Advances the confetti by one tick. While `raining`, spawns a new piece on
   each side every second tick. Each piece drifts, flutters left/right (a
   +/-30 sixteenths sway that flips every 4 ticks, offset per piece by `flip`)
   and falls; it dies when it reaches the top of the score panel, so confetti
   never covers the numbers.
   Returns whether any confetti is still on screen. */
static int drizzle_update(int raining)
{
   ++drizzle_tick;
   if (raining && drizzle_tick % 2 == 0)
   {
      /* Left and right of the mascot's circle (about x 545-1375). */
      drizzle_spawn(140, 520);
      drizzle_spawn(1400, 1780);
   }
   int any = 0;
   for (int index = 0; index < DRIZZLE_COUNT; ++index)
   {
      struct drizzle *piece = &drizzle[index];
      if (!piece->live)
         continue;
      int flutter = (piece->flip + drizzle_tick / 4) % 8 < 4 ? 30 : -30;
      piece->x += piece->vx + flutter;
      piece->y += piece->vy;
      if (piece->y / 16 + piece->height >= DRIZZLE_FLOOR)
         piece->live = false;
      else
         any = 1;
   }
   return any;
}

/* Draws every live confetti piece as a filled rectangle centred on its x. */
static void drizzle_draw(void *renderer)
{
   for (int index = 0; index < DRIZZLE_COUNT; ++index)
   {
      const struct drizzle *piece = &drizzle[index];
      if (!piece->live)
         continue;
      /* Pieces narrow and widen as they tumble: an 8-step cycle of
         full, full, half, 4 px edge-on, 4 px, half, full, full width. */
      unsigned turn = (piece->flip + drizzle_tick / 2) % 8;
      int width = turn < 2 || turn > 5 ? piece->width :
         turn == 2 || turn == 5 ? piece->width / 2 : 4;
      color(renderer, piece->red, piece->green, piece->blue);
      fill(renderer, piece->x / 16 - width / 2, piece->y / 16, width, piece->height);
   }
}

/* Draws one complete backglass frame from a state snapshot and presents it.
   Every frame is drawn from scratch (artwork, confetti, panel, text), which
   is simple and, since we draw rarely, cheap enough.
   Layout (1920x1080): the score panel spans x 130-1790, y 735-1030, with
   yellow rules top and bottom and a cyan divider at x 960. SCORE is centred
   on x 544 (left half) and BEST on x 1376 (right half). These numbers match
   the panel drawn by make_backglass_art.swift --sample-scores (which uses
   bottom-up AppKit coordinates: its y 50-345 is our y 735-1030). */
static void draw_state(void *renderer, void *artwork, void **font_textures,
                       const struct backglass_state *state)
{
   /* NULL source and destination rectangles = whole texture to whole window. */
   if (artwork)
      SDL_RenderCopy(renderer, artwork, 0, 0);
   else
   {
      color(renderer, 5, 11, 33);
      fill(renderer, 0, 0, 1920, 1080);
   }
   drizzle_draw(renderer);

   /* Score panel: dark box, then 8-pixel yellow rules on its top and bottom. */
   color(renderer, 10, 27, 51);
   fill(renderer, 130, 735, 1660, 295);
   color(renderer, 247, 220, 36);
   fill(renderer, 130, 735, 1660, 8);
   fill(renderer, 130, 1022, 1660, 8);
   /* While initials are being entered, a magenta banner across the whole
      panel replaces the two labels. */
   if (state->flags & (SNAKE_STATE_ENTERING_NAME | SNAKE_STATE_TOP_TEN))
   {
      const char *banner = state->flags & SNAKE_STATE_TOP_TEN ?
         "TOP TEN SCORE" : "NEW HIGH SCORE";
      draw_smooth_text(renderer, font_textures, banner,
                       960 - text_width(banner, 82) / 2, 760, 82, 98,
                       255, 80, 200);
   }
   else
   {
      /* Labels are centred over each half; BEST carries the holder's initials. */
      /* A small local array initialised from a string literal; it is short
         enough that the compiler stores it with plain moves, no memcpy. */
      char best_label[] = "BEST XXX";
      if (state->name[0])
         for (int index = 0; index < 3; ++index)
            best_label[5 + index] = state->name[index];
      else
         best_label[4] = 0; /* no initials yet: cut the string to "BEST" */
      draw_smooth_text(renderer, font_textures, "SCORE",
                       544 - text_width("SCORE", 82) / 2, 760, 82, 98,
                       247, 220, 36);
      draw_smooth_text(renderer, font_textures, best_label,
                       1376 - text_width(best_label, 82) / 2, 760, 82, 98,
                       247, 220, 36);
   }
   /* The divider starts below the banner while initials are being entered. */
   color(renderer, 0, 217, 246);
   if (state->flags & (SNAKE_STATE_ENTERING_NAME | SNAKE_STATE_TOP_TEN))
      fill(renderer, 957, 860, 6, 140);
   else
      fill(renderer, 957, 765, 6, 235);

   /* The numbers: 4 digits, 5 above 9999 (snake_score_text needs 6 bytes).
      The color() calls are only used by the pixel-font fallback; the smooth
      path tints with the RGB arguments instead. */
   char score[6];
   char best[6];
   snake_score_text(score, state->score);
   snake_score_text(best, state->best);
   color(renderer, 255, 255, 255);
   draw_smooth_text(renderer, font_textures, score,
                    544 - text_width(score, 118) / 2, 855, 118, 150,
                    255, 255, 255);
   color(renderer, 60, 238, 144);
   draw_smooth_text(renderer, font_textures, best,
                    1376 - text_width(best, 118) / 2, 855, 118, 150,
                    60, 238, 144);
   SDL_RenderPresent(renderer);
}

/* Shuts SDL down (restoring the display) and ends the process with `code`.
   There is no libc exit(), so we make the exit system call ourselves.
   SYS_exit (93) ends the calling thread; exit_group (94) would end every
   thread. The helper's own code is single-threaded and SDL_Quit() has already
   stopped SDL's subsystems, so ending this thread ends the process. */
__attribute__((noreturn)) static void finish(int code)
{
   SDL_Quit();
   system_call(SYS_exit, code, 0, 0);
   __builtin_unreachable(); /* the syscall does not return */
}

/* Program entry point. Without libc there is no crt0 to call main(), so the
   kernel jumps straight here (after the dynamic linker has loaded SDL2). It
   must never return - there is nothing to return to - hence noreturn and the
   explicit exit in finish(). Exit codes 1-3 identify which SDL setup step
   failed. */
__attribute__((noreturn)) void _start(void)
{
   print_text("ALP SNAKE BACKGLASS\n");
   /* 0x20 = SDL_INIT_VIDEO. The environment variable ForceConnectID=93, set by
      the core when it launched us, steers the cabinet's SDL build to the
      backglass display. */
   if (SDL_Init(0x20) != 0)
      finish(1);
   /* 0x1fff0000 = SDL_WINDOWPOS_UNDEFINED for x and y; flag 1 =
      SDL_WINDOW_FULLSCREEN. 1920x1080 is the backglass resolution. */
   void *window = SDL_CreateWindow("ALP Snake Backglass", 0x1fff0000, 0x1fff0000,
                                   1920, 1080, 1);
   if (!window)
      finish(2);
   /* -1 = first driver that works; flags 0 = no special requirements. */
   void *renderer = SDL_CreateRenderer(window, -1, 0);
   if (!renderer)
   {
      SDL_DestroyWindow(window);
      finish(3);
   }

   /* Load the background art. It is a BMP because plain SDL2 can load BMP
      without the SDL_image library. The path is relative to the cartridge
      folder, which is our working directory (inherited from exec.sh via the
      core). The 1 passed to SDL_LoadBMP_RW tells SDL to close `source` for
      us. The surface (CPU memory) is turned into a texture (GPU memory) once,
      then freed. Missing art is not fatal: draw_state() paints a plain
      dark background instead. */
   void *source = SDL_RWFromFile("./boxart/snake-backglass.bmp", "rb");
   void *surface = source ? SDL_LoadBMP_RW(source, 1) : 0;
   void *artwork = surface ? SDL_CreateTextureFromSurface(renderer, surface) : 0;
   if (surface)
      SDL_FreeSurface(surface);
   if (!artwork)
      print_text("ALP SNAKE BACKGLASS ART MISSING\n");

   /* Static, so they start zeroed without a memset call (no libc here). */
   static void *font_textures[36];
   make_font_textures(renderer, font_textures);

   /* Redraw only when something shown changes: score, best, flags (banner)
      or initials, plus every frame while confetti is moving. A slow periodic
      refresh (60 polls x 33 ms, about every 2 s) restores the display if
      anything else draws over it. Otherwise we just sleep, which keeps the
      helper's CPU use near zero. */
   static struct backglass_state state;
   uint32_t shown_score = 0;
   uint32_t shown_best = 0;
   uint32_t shown_flags = 0;
   static char shown_name[4];
   int drawn = 0;
   unsigned idle_polls = 0;
   for (;;)
   {
      int status = read_state(&state);
      if (status < 0)
         break; /* state file deleted: the core has unloaded, so quit */
      ++idle_polls;
      bool name_changed = false;
      for (int index = 0; index < 4; ++index)
         if (state.name[index] != shown_name[index])
            name_changed = true;
      /* Keep redrawing one frame past the last piece so none is left behind. */
      static int was_drizzling;
      int still_drizzling = drizzle_update(status > 0 &&
                                           (state.flags & SNAKE_STATE_ENTERING_NAME));
      int drizzling = still_drizzling || was_drizzling;
      was_drizzling = still_drizzling;
      if (status > 0 && (!drawn || state.score != shown_score ||
                         state.best != shown_best || state.flags != shown_flags ||
                         name_changed || drizzling || idle_polls >= 60))
      {
         draw_state(renderer, artwork, font_textures, &state);
         shown_score = state.score;
         shown_best = state.best;
         shown_flags = state.flags;
         for (int index = 0; index < 4; ++index)
            shown_name[index] = state.name[index];
         drawn = 1;
         idle_polls = 0;
      }
      SDL_Delay(33); /* poll about 30 times a second */
   }
   /* Orderly teardown: textures, then renderer, then window, then SDL. */
   if (artwork)
      SDL_DestroyTexture(artwork);
   for (int index = 0; index < 36; ++index)
      if (font_textures[index])
         SDL_DestroyTexture(font_textures[index]);
   SDL_DestroyRenderer(renderer);
   SDL_DestroyWindow(window);
   finish(0);
}
