/*
 * snake_game.h - the rules of ALP Snake, with no hardware in sight.
 *
 * This header (and snake_game.c) is the game's "rules engine": the board, the
 * snake, the food, the mouse, the SLOW road sign, the hawk, the speed levels
 * and the attract-mode autopilot. It knows nothing about libretro, pixels,
 * sound, the trackball, the clock or the cabinet. The front end
 * (snake_libretro.c) owns one struct snake_game, feeds it player input with
 * snake_steer(), calls snake_step() whenever its clock says the snake is due
 * to move, and then draws and plays sounds based on what the returned events
 * and the struct's fields say happened.
 *
 * Key ideas for a learner:
 *  - Keeping the rules separate from video/audio/input makes them easy to
 *    test. snake_game.c only needs <stdint.h>, so it compiles unchanged on a
 *    desktop PC: you can write a tiny host program that plays thousands of
 *    games with snake_demo_direction(), run it under a fuzzer or sanitizers
 *    (-fsanitize=address,undefined) feeding random steer calls, and check
 *    invariants (snake stays on the board, never overlaps itself, score never
 *    exceeds SNAKE_MAX_SCORE) - all without a cabinet or an emulator.
 *  - The engine is deterministic: given the same seed and the same sequence of
 *    snake_steer()/snake_step() calls it plays exactly the same game, which
 *    makes bugs reproducible.
 *  - Time is measured in "steps" (snake moves), never in seconds. The front
 *    end decides how long a step lasts (it gets shorter as speed_level rises),
 *    so countdowns below are quoted as "roughly N seconds at the start speed".
 *  - Everything is plain fixed-size data (no malloc), which suits the
 *    freestanding, no-libc build the core uses.
 */
#ifndef ALP_SNAKE_GAME_H
#define ALP_SNAKE_GAME_H

#include <stdint.h>

/* Board size in cells. With the front end's 54-pixel cells this makes a
   972 x 1512 playfield on the 1080 x 1920 portrait screen. */
#define SNAKE_COLUMNS 18
#define SNAKE_ROWS 28
/* Number of cells on the board (504): also the longest the snake can grow,
   so it sizes the body array. Filling the whole board wins the game. */
#define SNAKE_CELLS (SNAKE_COLUMNS * SNAKE_ROWS)

/* The four headings. The order matters: they go clockwise, so turning right
   is +1 mod 4, turning left is +3 mod 4, and two directions are opposite
   exactly when (a ^ b) == 2 (UP=0/DOWN=2, RIGHT=1/LEFT=3). */
enum snake_direction {
   SNAKE_UP,
   SNAKE_RIGHT,
   SNAKE_DOWN,
   SNAKE_LEFT
};

/* The overall state of a game. */
enum snake_phase {
   SNAKE_READY,      /* initialised but not started (attract / title screen) */
   SNAKE_PLAYING,    /* snake_step() moves the snake */
   SNAKE_PAUSED,     /* frozen; snake_pause() toggles back to PLAYING */
   SNAKE_GAME_OVER,  /* hit a wall, itself, or the hawk */
   SNAKE_WON         /* the snake filled every cell */
};

/* What snake_step() reports back for the snake itself. */
enum snake_event {
   SNAKE_EVENT_NONE,  /* nothing happened (not playing) */
   SNAKE_EVENT_MOVE,  /* moved one cell */
   SNAKE_EVENT_EAT,   /* moved onto the food and grew */
   SNAKE_EVENT_DIE,   /* crashed or was caught by the hawk; phase is GAME_OVER */
   SNAKE_EVENT_WIN    /* filled the board; phase is WON */
};

/* What happened to the mouse on the latest step (game->mouse_event).
   Kept separate from snake_event because several things can happen on the
   same step, e.g. eating food while a mouse escapes. */
enum snake_mouse_event {
   SNAKE_MOUSE_NONE,
   SNAKE_MOUSE_APPEARED,  /* a mouse just ran onto the board */
   SNAKE_MOUSE_CAUGHT,    /* the snake caught it (+SNAKE_MOUSE_POINTS) */
   SNAKE_MOUSE_ESCAPED    /* it ran off the far edge */
};

/* Segments the snake has at the start of a game (and the shortest a mouse
   can shrink it to). */
#define SNAKE_START_LENGTH 4
/* Speed rises one level every few foods, up to a maximum; a slow sign
   takes one level off. The front end maps level 0..4 to 8..4 frames per
   move. */
#define SNAKE_FOODS_PER_LEVEL 5
#define SNAKE_MAX_SPEED_LEVEL 4

/* What happened to the SLOW sign on the latest step (game->sign_event). */
enum snake_sign_event {
   SNAKE_SIGN_NONE,
   SNAKE_SIGN_APPEARED,  /* a sign was put on the board */
   SNAKE_SIGN_EATEN,     /* the snake ran over it and lost a speed level */
   SNAKE_SIGN_EXPIRED    /* it was not collected in time and vanished */
};

/* Where the hawk is in its attack (game->hawk_phase). */
enum snake_hawk_phase {
   SNAKE_HAWK_AWAY,      /* not on screen; hawk_countdown runs down */
   SNAKE_HAWK_CIRCLING,  /* its shadow circles (hawk_angle) as a warning */
   SNAKE_HAWK_DIVING     /* locked on to hawk_target; strikes when it lands */
};

/* What the hawk did on the latest step (game->hawk_event). */
enum snake_hawk_event {
   SNAKE_HAWK_NONE,
   SNAKE_HAWK_ARRIVED,  /* started circling */
   SNAKE_HAWK_DIVES,    /* picked its target square and began the dive */
   SNAKE_HAWK_MISSED,   /* struck but the head was outside the 3x3 zone */
   SNAKE_HAWK_CAUGHT    /* struck the head: game over */
};

/* Moves the shadow circles before diving, and moves from locking on to the
   strike, at the starting speed. Both grow with the speed level (+4 and +2
   moves per level) so the warning lasts about the same real time at any
   speed (roughly 2.4 s of circling and 1.1 s to dodge). The strike hits a
   3x3 square aimed where the head was heading. */
#define SNAKE_HAWK_CIRCLE_STEPS 18
#define SNAKE_HAWK_DIVE_STEPS 8
/* Bonus for surviving a dive. */
#define SNAKE_HAWK_DODGE_POINTS 50

/* Optional extras, switched on and off from the cabinet's options menu.
   They are bit flags combined into game->features. */
#define SNAKE_FEATURE_MOUSE 1u
#define SNAKE_FEATURE_SIGNS 2u
#define SNAKE_FEATURE_HAWK 4u
#define SNAKE_FEATURES_ALL 7u

/* Points for ordinary food and for a caught mouse. */
#define SNAKE_FOOD_POINTS 10
#define SNAKE_MOUSE_POINTS 20
/* Segments a caught mouse takes off the tail (never below the start length). */
#define SNAKE_MOUSE_SHRINK 3
/* Mice keep the snake short, so scores have no natural ceiling; this is the
   largest the five-digit displays and the save file accept. */
#define SNAKE_MAX_SCORE 99990u

/* A board cell. int8_t is plenty (the board is at most 28 cells across) and
   keeps the 504-entry body array small. x grows rightwards, y downwards. */
struct snake_point {
   int8_t x;
   int8_t y;
};

/* The complete state of one game. The front end may read any field (to draw
   the board, play sounds or fill the backglass state) but changes it only
   through the functions below, apart from `features` and `best`, which it
   sets from saved settings and the high-score table. */
struct snake_game {
   /* body[0] is the head, body[length - 1] the tail. Moving shifts every
      segment down one slot (a simple array, not a ring buffer: at most 504
      two-byte copies per move, which is cheap). */
   struct snake_point body[SNAKE_CELLS];
   struct snake_point food;            /* the one piece of food on the board */
   unsigned length;                    /* segments in use in body[] */
   unsigned score;                     /* this game's score, capped */
   unsigned best;                      /* highest score seen (front end seeds it) */
   unsigned steps;                     /* moves made this game */
   uint32_t random_state;              /* LCG state; never 0 after snake_init */
   enum snake_direction direction;     /* heading used for the last move */
   /* The heading the snake will have after the queued turns are applied;
      new turns are checked against this, so you cannot reverse into
      yourself by pressing two keys within one move. */
   enum snake_direction pending_direction;
   /* Up to two turns pressed since the last move, oldest first. Queuing lets
      quick "right then down" inputs both count instead of the second one
      overwriting the first before the snake has moved. */
   enum snake_direction queued_directions[2];
   unsigned queued_turns;              /* entries used in queued_directions */
   enum snake_phase phase;
   /* The occasional mouse that runs across the board. */
   struct snake_point mouse;
   enum snake_direction mouse_direction;  /* the edge it is running towards */
   unsigned mouse_active;              /* nonzero while a mouse is on the board */
   unsigned mouse_countdown;           /* moves until the next mouse appears */
   unsigned mouse_moves;               /* moves since it appeared (rests every 3rd) */
   unsigned shrink_pending;            /* tail segments still to pull in */
   /* Food eaten this game; the snake's speed is based on this, so catching
      mice scores points without speeding the game up. */
   unsigned food_eaten;
   unsigned speed_level;               /* 0 .. SNAKE_MAX_SPEED_LEVEL */
   unsigned foods_toward_level;        /* food eaten since the last level change */
   /* The occasional SLOW road sign. */
   struct snake_point sign;
   unsigned sign_active;               /* nonzero while a sign is on the board */
   unsigned sign_steps_left;           /* moves before an uncollected sign fades */
   unsigned sign_countdown;            /* moves until the next sign appears */
   enum snake_sign_event sign_event;   /* what the sign did on the latest step */
   /* The rare hawk. */
   enum snake_hawk_phase hawk_phase;
   unsigned hawk_countdown;            /* moves until it next arrives (AWAY) */
   unsigned hawk_steps_left;           /* moves left in CIRCLING or DIVING */
   unsigned hawk_dive_steps;           /* total length of the current dive,
                                          so the front end can draw progress */
   unsigned hawk_angle;                /* 0..15 position of the circling shadow
                                          (sixteenths of a turn; drawing only) */
   struct snake_point hawk_target;     /* centre of the 3x3 strike zone; kept
                                          one cell from the edges so the zone
                                          always fits on the board */
   enum snake_hawk_event hawk_event;   /* what the hawk did on the latest step */
   unsigned features;                  /* SNAKE_FEATURE_* bits that are enabled */
   /* What happened to the mouse on the latest step. */
   enum snake_mouse_event mouse_event;
};

/* Resets everything, including `best`, and seeds the random generator
   (a zero seed is replaced by 1). Leaves the game in SNAKE_READY with all
   features enabled. Call once at start-up. */
void snake_init(struct snake_game *game, uint32_t seed);
/* Begins a new game: a 4-segment snake heading up from the centre, fresh
   food, and new timers for the mouse, sign and hawk. Keeps `best`,
   `features` and the random state, so successive games differ. */
void snake_start(struct snake_game *game);
/* Toggles between SNAKE_PLAYING and SNAKE_PAUSED; does nothing otherwise. */
void snake_pause(struct snake_game *game);
/* Queues a turn for the next move. Returns 1 if accepted, 0 if ignored
   (not playing, two turns already queued, same heading, or a reversal). */
int snake_steer(struct snake_game *game, enum snake_direction direction);
/* Advances the game by one move: applies a queued turn, moves the snake,
   handles food, mouse, sign and hawk, and returns what happened to the
   snake. The mouse/sign/hawk *_event fields describe the rest. */
enum snake_event snake_step(struct snake_game *game);
/* The attract-mode autopilot: a direction that heads for food (or a nearby
   mouse) without walking into a dead end. */
enum snake_direction snake_demo_direction(const struct snake_game *game);

#endif
