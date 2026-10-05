/*
 * snake_backglass_state.h - the tiny "mailbox" between the game and the
 * backglass helper.
 *
 * The cabinet has two screens. The libretro core (snake_libretro.c) draws the
 * playfield through Retroplayer, but it has no way to draw on the second
 * (backglass) screen. So the core starts a separate SDL2 program,
 * alp_snake_backglass (snake_backglass.c), and the two talk through one small
 * binary file in /tmp:
 *
 *   core:   fill a struct backglass_state, write it to SNAKE_STATE_TEMP_PATH,
 *           then rename() it over SNAKE_STATE_PATH. rename is atomic, so the
 *           reader never sees a half-written file.
 *   helper: poll SNAKE_STATE_PATH about 30 times a second and redraw when the
 *           values it shows change. When the file disappears (the core deletes
 *           it on unload) the helper exits.
 *
 * Both programs are built from this header with the same compiler for the
 * same CPU, so the struct is simply written and read as raw bytes (all fields
 * are 4-byte aligned, so there is no padding to worry about). The magic
 * number lets the reader reject a file that is truncated or from something
 * else. If you add a field, rebuild both programs together and consider
 * changing the magic.
 */
#ifndef ALP_SNAKE_BACKGLASS_STATE_H
#define ALP_SNAKE_BACKGLASS_STATE_H

#include <stdint.h>

/* "SNA2" in ASCII: identifies a valid state file (and its layout version). */
#define SNAKE_STATE_MAGIC 0x534e4132
/* Bits for backglass_state.flags. */
/* entering initials for a new best (first-place) score: banner plus confetti */
#define SNAKE_STATE_ENTERING_NAME 1u
#define SNAKE_STATE_TOP_TEN 2u   /* entering initials for a lower top-ten place */
/* Lives in /tmp: it is scratch data that need not survive a reboot. */
#define SNAKE_STATE_PATH "/tmp/alp-snake-state.bin"
#define SNAKE_STATE_TEMP_PATH "/tmp/alp-snake-state.tmp"

/* One snapshot of the game, as written by write_backglass_state() in the core.
   The helper currently only draws score, best, name and flags; the other
   fields are filled in by the core but unused (handy for experiments/debugging). */
struct backglass_state {
   uint32_t magic;     /* SNAKE_STATE_MAGIC */
   uint32_t score;     /* current score (last game's score during attract mode) */
   uint32_t best;      /* best score in the high score table */
   uint32_t phase;     /* game.phase (attract, ready, playing, ...) */
   uint32_t direction; /* snake heading */
   uint32_t length;    /* snake length */
   uint32_t steps;     /* moves made this game */
   uint32_t trackball; /* 1 if the core found the trackball input device */
   uint32_t sequence;  /* core frame counter at the time of writing */
   char name[4];       /* 3 initials + NUL: the initials being typed while a
                          flag below is set, otherwise the best score holder's
                          (empty when the best has no name yet) */
   uint32_t flags;     /* SNAKE_STATE_* bits above */
};

#endif
