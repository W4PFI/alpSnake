/*
 * snake_game.c - the hardware-independent rules engine for ALP Snake.
 *
 * Everything that decides *what happens* in a game lives here: moving the
 * snake, growing and shrinking it, placing food, the mouse that runs across
 * the board, the SLOW road sign, the hawk, speed levels, scoring and the
 * attract-mode autopilot. Nothing here draws, makes sound, reads input or
 * looks at a clock; snake_libretro.c does all of that and simply calls
 * snake_steer() and snake_step() (see snake_game.h for the overview).
 *
 * Key ideas for a learner:
 *  - Why keep the rules separate? Because this file has no dependencies
 *    beyond <stdint.h>, it builds with any C compiler on any machine. You can
 *    compile it on your PC together with a small test program, or feed it to
 *    a fuzzer (libFuzzer/AFL: turn the input bytes into steer calls and
 *    steps) and sanitizers, and catch out-of-range array writes or rule bugs
 *    in seconds - long before you copy a .UCE to the cabinet.
 *  - All randomness comes from one seeded LCG kept in the struct, so a game
 *    can be replayed exactly from its seed and inputs.
 *  - Time is counted in snake moves ("steps"). Every countdown here (mouse,
 *    sign, hawk) ticks once per move; the front end decides how many video
 *    frames a move takes for the current speed level.
 *  - Freestanding-friendly: no libc calls, no malloc, and no large struct
 *    copies or `= {0}` initialisers (which could make the compiler call
 *    memcpy/memset, functions that do not exist in the core's no-libc build).
 *    Small struct snake_point copies are just two bytes and are fine.
 *  - Per-step "events" (mouse_event, sign_event, hawk_event) are cleared at
 *    the start of every snake_step() and set when something happens, so the
 *    front end can play the matching sound or animation once.
 */
#include "snake_game.h"

/* A linear congruential generator (the classic "Numerical Recipes"
   constants 1664525 / 1013904223, modulo 2^32 via unsigned overflow). It is
   tiny, deterministic and needs no libc. An LCG's LOW bits are poor: bit 0
   simply alternates 0,1,0,1... An earlier version returned the whole state,
   so two consecutive `% 18` and `% 28` draws always had opposite parity and
   SLOW signs could only land on half the squares. Returning only the top 16
   bits (the well-mixed ones) fixes that. 16 bits is plenty: every caller
   takes `% n` with n of at most a few hundred, or tests a single bit. */
static uint32_t next_random(struct snake_game *game)
{
   game->random_state = game->random_state * 1664525u + 1013904223u;
   return game->random_state >> 16;
}

/* Returns 1 if (x, y) is one of the first `count` body segments (head
   first). Passing length - 1 ignores the tail, which is useful when asking
   "will this cell be free after the snake moves?". A linear scan of at most
   504 cells is fast enough for a once-per-move game. */
static int occupied(const struct snake_game *game, int x, int y, unsigned count)
{
   for (unsigned segment = 0; segment < count; ++segment)
      if (game->body[segment].x == x && game->body[segment].y == y)
         return 1;
   return 0;
}

/* Puts the food on a uniformly random free cell (not on the snake, not on the
   sign). It picks the n-th free cell, counting in reading order, rather than
   retrying random cells, so it always finishes quickly even when the board is
   almost full. The mouse is not excluded; food and mouse may share a cell. */
static void place_food(struct snake_game *game)
{
   unsigned remaining = SNAKE_CELLS - game->length -
      (game->sign_active ? 1u : 0u);
   if (!remaining)
   {
      /* Only the sign's square is free: the sign makes way for the food. */
      if (game->sign_active)
      {
         game->sign_active = 0;
         game->food = game->sign;
      }
      return;
   }
   unsigned target = next_random(game) % remaining;
   for (int y = 0; y < SNAKE_ROWS; ++y)
      for (int x = 0; x < SNAKE_COLUMNS; ++x)
         if (!occupied(game, x, y, game->length) &&
             !(game->sign_active && x == game->sign.x && y == game->sign.y))
         {
            if (!target)
            {
               game->food.x = (int8_t)x;
               game->food.y = (int8_t)y;
               return;
            }
            --target;
         }
}

/* Returns 1 if (x, y) is on the board. */
static int inside(int x, int y)
{
   return x >= 0 && x < SNAKE_COLUMNS && y >= 0 && y < SNAKE_ROWS;
}

/* Moves (x, y) one cell in `direction`. y grows downwards, so UP is -1. */
static void step_point(enum snake_direction direction, int *x, int *y)
{
   if (direction == SNAKE_UP)
      --*y;
   else if (direction == SNAKE_RIGHT)
      ++*x;
   else if (direction == SNAKE_DOWN)
      ++*y;
   else
      --*x;
}

/* Steps until the next mouse: roughly 10-20 seconds at the starting speed. */
static void schedule_mouse(struct snake_game *game)
{
   /* 75..150 moves; at 8 frames per move (60 fps) that is 10-20 s. */
   game->mouse_countdown = 75 + next_random(game) % 76;
}

/* Adds to the score, capping it at SNAKE_MAX_SCORE so it always fits the
   five-digit displays and save format, and keeps `best` up to date. */
static void add_points(struct snake_game *game, unsigned points)
{
   game->score += points;
   if (game->score > SNAKE_MAX_SCORE)
      game->score = SNAKE_MAX_SCORE;
   if (game->score > game->best)
      game->best = game->score;
}

/* Catching a mouse scores and makes the tail pull in over the next moves. */
static void catch_mouse(struct snake_game *game)
{
   game->mouse_active = 0;
   game->mouse_event = SNAKE_MOUSE_CAUGHT;
   game->shrink_pending += SNAKE_MOUSE_SHRINK;
   add_points(game, SNAKE_MOUSE_POINTS);
   schedule_mouse(game);
}

/* A mouse enters from one edge and runs for the opposite one, more often
   across the board (the short way) than along it. */
static void spawn_mouse(struct snake_game *game)
{
   /* Try a few random entry points; if none is acceptable, try again in
      20 moves instead of looping forever on a crowded board. */
   for (int attempt = 0; attempt < 8; ++attempt)
   {
      int x;
      int y;
      enum snake_direction direction;
      uint32_t choice = next_random(game);
      /* 70% of mice run left/right (18 cells, the short way across the
         portrait board), 30% up/down. Bit 4 of the same random number picks
         which of the two edges it starts from. */
      if (choice % 10 < 7)
      {
         direction = choice & 16 ? SNAKE_RIGHT : SNAKE_LEFT;
         x = direction == SNAKE_RIGHT ? 0 : SNAKE_COLUMNS - 1;
         y = (int)(next_random(game) % SNAKE_ROWS);
      }
      else
      {
         direction = choice & 16 ? SNAKE_DOWN : SNAKE_UP;
         y = direction == SNAKE_DOWN ? 0 : SNAKE_ROWS - 1;
         x = (int)(next_random(game) % SNAKE_COLUMNS);
      }
      int dx = x - game->body[0].x;
      int dy = y - game->body[0].y;
      /* Reject spots within 5 cells (Manhattan distance) of the head, so a
         mouse never pops up right in the snake's mouth, and cells taken by
         the snake or the food. */
      if ((dx < 0 ? -dx : dx) + (dy < 0 ? -dy : dy) < 5 ||
          occupied(game, x, y, game->length) ||
          (x == game->food.x && y == game->food.y))
         continue;
      game->mouse.x = (int8_t)x;
      game->mouse.y = (int8_t)y;
      game->mouse_direction = direction;
      game->mouse_active = 1;
      game->mouse_moves = 0;
      game->mouse_event = SNAKE_MOUSE_APPEARED;
      return;
   }
   game->mouse_countdown = 20;
}

/* Runs after the snake has moved. The mouse rests every third step, so the
   snake can head it off but not run it down from behind. */
static void update_mouse(struct snake_game *game)
{
   if (!(game->features & SNAKE_FEATURE_MOUSE))
   {
      game->mouse_active = 0;
      return;
   }
   if (!game->mouse_active)
   {
      if (game->mouse_countdown)
         --game->mouse_countdown;
      else
         spawn_mouse(game);
      return;
   }
   /* Skip every third move: the mouse runs at two thirds of the snake's
      speed. */
   if (++game->mouse_moves % 3 == 0)
      return;

   int x = game->mouse.x;
   int y = game->mouse.y;
   /* The two directions at right angles to its run (+1 and +3 mod 4 are a
      right and a left turn), shuffled so neither side is favoured. */
   enum snake_direction sideways[2] = {
      (enum snake_direction)((game->mouse_direction + 1) % 4),
      (enum snake_direction)((game->mouse_direction + 3) % 4)
   };
   if (next_random(game) & 1)
   {
      sideways[0] = sideways[1];
      sideways[1] = (enum snake_direction)((game->mouse_direction + 1) % 4);
   }
   enum snake_direction direction = game->mouse_direction;
   /* An occasional sideways jink keeps it lively (about 1 move in 6). */
   if (next_random(game) % 6 == 0)
   {
      int jx = x;
      int jy = y;
      step_point(sideways[0], &jx, &jy);
      if (inside(jx, jy) && !occupied(game, jx, jy, game->length))
         direction = sideways[0];
   }
   int nx = x;
   int ny = y;
   step_point(direction, &nx, &ny);
   /* Running off the board means it escaped (normally out the far edge). */
   if (!inside(nx, ny))
   {
      game->mouse_active = 0;
      game->mouse_event = SNAKE_MOUSE_ESCAPED;
      schedule_mouse(game);
      return;
   }
   if (nx == game->body[0].x && ny == game->body[0].y)
   {
      /* Ran straight into the snake's mouth. */
      catch_mouse(game);
      return;
   }
   if (occupied(game, nx, ny, game->length))
   {
      /* Blocked by the body: dart to one side, or wait. */
      for (int side = 0; side < 2; ++side)
      {
         nx = x;
         ny = y;
         step_point(sideways[side], &nx, &ny);
         if (inside(nx, ny) && !occupied(game, nx, ny, game->length))
         {
            game->mouse.x = (int8_t)nx;
            game->mouse.y = (int8_t)ny;
            return;
         }
      }
      return;
   }
   game->mouse.x = (int8_t)nx;
   game->mouse.y = (int8_t)ny;
}

/* Steps until the next slow sign: roughly 20-35 seconds at the start speed. */
static void schedule_sign(struct snake_game *game)
{
   /* 150..259 moves. The countdown only runs while speed_level > 0. */
   game->sign_countdown = 150 + next_random(game) % 110;
}

/* Moves an uncollected sign stays on the board: about 5-8 s, since signs
   only appear at speed level 1 or above (7 down to 4 frames per move). */
#define SIGN_LIFETIME 70

/* Places a SLOW sign on a random free cell at least 4 cells from the head
   and away from the food and mouse. Gives up after 12 tries and retries in
   20 moves. */
static void spawn_sign(struct snake_game *game)
{
   for (int attempt = 0; attempt < 12; ++attempt)
   {
      int x = (int)(next_random(game) % SNAKE_COLUMNS);
      int y = (int)(next_random(game) % SNAKE_ROWS);
      int dx = x - game->body[0].x;
      int dy = y - game->body[0].y;
      if ((dx < 0 ? -dx : dx) + (dy < 0 ? -dy : dy) < 4 ||
          occupied(game, x, y, game->length) ||
          (x == game->food.x && y == game->food.y) ||
          (game->mouse_active && x == game->mouse.x && y == game->mouse.y))
         continue;
      game->sign.x = (int8_t)x;
      game->sign.y = (int8_t)y;
      game->sign_active = 1;
      game->sign_steps_left = SIGN_LIFETIME;
      game->sign_event = SNAKE_SIGN_APPEARED;
      return;
   }
   game->sign_countdown = 20;
}

/* Signs only appear once there is speed to lose, and fade after a while. */
static void update_sign(struct snake_game *game)
{
   if (!(game->features & SNAKE_FEATURE_SIGNS))
   {
      game->sign_active = 0;
      return;
   }
   if (game->sign_active)
   {
      if (--game->sign_steps_left == 0)
      {
         game->sign_active = 0;
         game->sign_event = SNAKE_SIGN_EXPIRED;
         schedule_sign(game);
      }
      return;
   }
   if (game->speed_level == 0)
      return;
   if (game->sign_countdown)
      --game->sign_countdown;
   else
      spawn_sign(game);
}

/* Moves until the hawk next appears: roughly 60-100 seconds at the start
   speed, so it stays a rare event. */
static void schedule_hawk(struct snake_game *game)
{
   /* 450..749 moves. */
   game->hawk_countdown = 450 + next_random(game) % 300;
}

/* Limits value to the range low..high. */
static int clamp(int value, int low, int high)
{
   return value < low ? low : value > high ? high : value;
}

/* The hawk is a three-stage state machine advanced once per move:
   AWAY (count down to arrival) -> CIRCLING (a warning shadow circles for
   SNAKE_HAWK_CIRCLE_STEPS + 4 per speed level moves) -> DIVING (it picks a
   target where the head will be if it keeps going straight, and strikes a
   3x3 square there after SNAKE_HAWK_DIVE_STEPS + 2 per speed level moves).
   Turning away in time dodges it for a bonus. */
/* Returns nonzero if the hawk caught the snake on this move. */
static int update_hawk(struct snake_game *game)
{
   if (!(game->features & SNAKE_FEATURE_HAWK))
   {
      /* Switched off: call off any attack in progress. */
      if (game->hawk_phase != SNAKE_HAWK_AWAY)
      {
         game->hawk_phase = SNAKE_HAWK_AWAY;
         schedule_hawk(game);
      }
      return 0;
   }
   if (game->hawk_phase == SNAKE_HAWK_AWAY)
   {
      if (game->hawk_countdown)
         --game->hawk_countdown;
      else
      {
         game->hawk_phase = SNAKE_HAWK_CIRCLING;
         game->hawk_steps_left = SNAKE_HAWK_CIRCLE_STEPS + 4 * game->speed_level;
         /* Start the shadow at a random sixteenth of the circle. */
         game->hawk_angle = next_random(game) % 16;
         game->hawk_event = SNAKE_HAWK_ARRIVED;
      }
      return 0;
   }
   if (game->hawk_phase == SNAKE_HAWK_CIRCLING)
   {
      game->hawk_angle = (game->hawk_angle + 1) % 16;
      if (--game->hawk_steps_left)
         return 0;
      /* Lock on to where the head will be if it keeps going straight. */
      game->hawk_dive_steps = SNAKE_HAWK_DIVE_STEPS + 2 * game->speed_level;
      int x = game->body[0].x;
      int y = game->body[0].y;
      for (unsigned step = 0; step < game->hawk_dive_steps; ++step)
         step_point(game->direction, &x, &y);
      /* Keep the target one cell in from the edges so the whole 3x3 strike
         zone is on the board (and the drawing never goes off it). */
      game->hawk_target.x = (int8_t)clamp(x, 1, SNAKE_COLUMNS - 2);
      game->hawk_target.y = (int8_t)clamp(y, 1, SNAKE_ROWS - 2);
      game->hawk_phase = SNAKE_HAWK_DIVING;
      game->hawk_steps_left = game->hawk_dive_steps;
      game->hawk_event = SNAKE_HAWK_DIVES;
      return 0;
   }
   if (--game->hawk_steps_left)
      return 0;
   /* Strike. */
   game->hawk_phase = SNAKE_HAWK_AWAY;
   schedule_hawk(game);
   /* Caught if the head is anywhere in the 3x3 square around the target. */
   int dx = game->body[0].x - game->hawk_target.x;
   int dy = game->body[0].y - game->hawk_target.y;
   if (dx >= -1 && dx <= 1 && dy >= -1 && dy <= 1)
   {
      game->hawk_event = SNAKE_HAWK_CAUGHT;
      return 1;
   }
   game->hawk_event = SNAKE_HAWK_MISSED;
   add_points(game, SNAKE_HAWK_DODGE_POINTS);
   return 0;
}

/* One-time setup: clears every field (field by field rather than with a
   struct initialiser, to avoid compiler-generated memset in the freestanding
   build) and seeds the random generator. A zero seed would still work with
   this LCG, but is replaced by 1 anyway. The body array is left as is;
   length 0 means none of it is in use. */
void snake_init(struct snake_game *game, uint32_t seed)
{
   game->length = 0;
   game->score = 0;
   game->best = 0;
   game->steps = 0;
   game->random_state = seed ? seed : 1;
   game->direction = SNAKE_UP;
   game->pending_direction = SNAKE_UP;
   game->queued_turns = 0;
   game->phase = SNAKE_READY;
   game->mouse_active = 0;
   game->mouse_countdown = 0;
   game->mouse_moves = 0;
   game->shrink_pending = 0;
   game->food_eaten = 0;
   game->speed_level = 0;
   game->foods_toward_level = 0;
   game->sign_active = 0;
   game->sign_steps_left = 0;
   game->sign_countdown = 0;
   game->sign_event = SNAKE_SIGN_NONE;
   game->hawk_phase = SNAKE_HAWK_AWAY;
   game->hawk_countdown = 0;
   game->hawk_steps_left = 0;
   game->hawk_dive_steps = SNAKE_HAWK_DIVE_STEPS;
   game->hawk_angle = 0;
   game->hawk_event = SNAKE_HAWK_NONE;
   game->mouse_event = SNAKE_MOUSE_NONE;
   game->features = SNAKE_FEATURES_ALL;
}

/* Starts a new game: the snake lies vertically in the middle of the board
   with its head at the top, heading up. `best`, `features` and the random
   state carry over, so each game is different. */
void snake_start(struct snake_game *game)
{
   game->length = SNAKE_START_LENGTH;
   game->score = 0;
   game->steps = 0;
   game->direction = SNAKE_UP;
   game->pending_direction = SNAKE_UP;
   game->queued_turns = 0;
   game->phase = SNAKE_PLAYING;
   game->mouse_active = 0;
   game->mouse_moves = 0;
   game->shrink_pending = 0;
   game->food_eaten = 0;
   game->speed_level = 0;
   game->foods_toward_level = 0;
   game->sign_active = 0;
   game->sign_event = SNAKE_SIGN_NONE;
   game->hawk_phase = SNAKE_HAWK_AWAY;
   game->hawk_event = SNAKE_HAWK_NONE;
   game->mouse_event = SNAKE_MOUSE_NONE;
   schedule_mouse(game);
   schedule_sign(game);
   schedule_hawk(game);
   for (unsigned segment = 0; segment < game->length; ++segment)
   {
      game->body[segment].x = SNAKE_COLUMNS / 2;
      game->body[segment].y = SNAKE_ROWS / 2 + (int)segment;
   }
   place_food(game);
}

/* Toggles pause; has no effect when the game is not running. */
void snake_pause(struct snake_game *game)
{
   if (game->phase == SNAKE_PLAYING)
      game->phase = SNAKE_PAUSED;
   else if (game->phase == SNAKE_PAUSED)
      game->phase = SNAKE_PLAYING;
}

/* Records a turn for the next move(s). Input arrives many times per move
   (every video frame), so instead of changing `direction` immediately the
   turn is queued; up to two turns can wait, so a quick "up then left" both
   happen on consecutive moves. A turn is ignored if it repeats the heading
   the snake will already have, or reverses it (the XOR-2 trick from
   snake_game.h), which would make the snake bite its own neck. Returns 1 if
   the turn was accepted, so the front end can play a click. */
int snake_steer(struct snake_game *game, enum snake_direction direction)
{
   if (game->phase != SNAKE_PLAYING || game->queued_turns == 2 ||
       direction == game->pending_direction ||
       ((unsigned)direction ^ (unsigned)game->pending_direction) == 2u)
      return 0;
   game->queued_directions[game->queued_turns++] = direction;
   game->pending_direction = direction;
   return 1;
}

/* Advances the game by exactly one move. Order matters here:
   1. clear last move's mouse/sign/hawk events;
   2. take the oldest queued turn;
   3. work out the new head cell and check for a crash;
   4. move the body, grow if eating, pull in the tail if shrinking;
   5. score the mouse / food / sign the head landed on;
   6. check for a win, place new food, then let the mouse, sign and hawk
      take their turn (the hawk can still end the game). */
enum snake_event snake_step(struct snake_game *game)
{
   game->mouse_event = SNAKE_MOUSE_NONE;
   game->sign_event = SNAKE_SIGN_NONE;
   game->hawk_event = SNAKE_HAWK_NONE;
   if (game->phase != SNAKE_PLAYING)
      return SNAKE_EVENT_NONE;

   if (game->queued_turns)
   {
      game->direction = game->queued_directions[0];
      --game->queued_turns;
      if (game->queued_turns)
         game->queued_directions[0] = game->queued_directions[1];
   }
   game->pending_direction = game->queued_turns ?
      game->queued_directions[0] : game->direction;
   int x = game->body[0].x;
   int y = game->body[0].y;
   step_point(game->direction, &x, &y);

   int eating = x == game->food.x && y == game->food.y;
   int catching = game->mouse_active && x == game->mouse.x && y == game->mouse.y;
   int slowing = game->sign_active && x == game->sign.x && y == game->sign.y;
   /* Crash test. The tail cell normally moves out of the way this move, so
      it is excluded - except when eating, because then the snake grows and
      the tail stays put. */
   if (!inside(x, y) ||
       occupied(game, x, y, game->length - (eating ? 0 : 1)))
   {
      game->phase = SNAKE_GAME_OVER;
      if (game->score > game->best)
         game->best = game->score;
      return SNAKE_EVENT_DIE;
   }

   /* Shift every segment back one slot and put the new head in front. When
      eating, length grew first, so the old tail is kept (the snake grows by
      one). This is a simple O(length) copy instead of a ring buffer; at most
      504 two-byte copies per move is nothing for the CPU. */
   if (eating)
      ++game->length;
   for (unsigned segment = game->length - 1; segment > 0; --segment)
      game->body[segment] = game->body[segment - 1];
   game->body[0].x = (int8_t)x;
   game->body[0].y = (int8_t)y;
   ++game->steps;

   /* The tail pulls in one extra segment per move while shrinking, but
      not on a move where the snake also eats. */
   if (!eating && game->shrink_pending)
   {
      if (game->length > SNAKE_START_LENGTH)
         --game->length;
      --game->shrink_pending;
   }
   if (catching)
      catch_mouse(game);
   if (eating)
   {
      ++game->food_eaten;
      add_points(game, SNAKE_FOOD_POINTS);
      if (++game->foods_toward_level >= SNAKE_FOODS_PER_LEVEL)
      {
         game->foods_toward_level = 0;
         if (game->speed_level < SNAKE_MAX_SPEED_LEVEL)
            ++game->speed_level;
      }
   }
   if (slowing)
   {
      /* Back one speed level; the next speed-up needs a fresh set of food. */
      game->sign_active = 0;
      game->sign_event = SNAKE_SIGN_EATEN;
      if (game->speed_level)
         --game->speed_level;
      game->foods_toward_level = 0;
      schedule_sign(game);
   }
   /* Every cell is snake: there is nowhere to put food, so the game is won. */
   if (game->length == SNAKE_CELLS)
   {
      game->mouse_active = 0;
      game->phase = SNAKE_WON;
      return SNAKE_EVENT_WIN;
   }
   if (eating)
      place_food(game);
   /* Skip the mouse/sign update on the move it was collected, so the next one
      is not counted down (or a new one placed) on the same move. */
   if (!catching)
      update_mouse(game);
   if (!slowing)
      update_sign(game);
   if (update_hawk(game))
   {
      game->phase = SNAKE_GAME_OVER;
      game->mouse_active = 0;
      return SNAKE_EVENT_DIE;
   }
   return eating ? SNAKE_EVENT_EAT : SNAKE_EVENT_MOVE;
}

/* ------------------------------------------------------------------------
   Attract-mode autopilot.
   ------------------------------------------------------------------------ */
/* Scratch space for the flood fill below. They are static (file-scope)
   rather than local arrays: about 1.5 KB that would otherwise sit on the
   stack each call, and static zero-initialised data needs no memset. This
   also means snake_demo_direction() is not re-entrant (fine for one game). */
static uint8_t demo_blocked[SNAKE_ROWS][SNAKE_COLUMNS];
static uint16_t demo_queue[SNAKE_CELLS];

/* Counts the open cells reachable from (x, y) after the snake moves there,
   stopping once `limit` is reached. This is a breadth-first flood fill:
   mark the snake's body (minus the tail, which will have moved, unless the
   snake is eating) and the new head as blocked, then repeatedly take a cell
   from the queue and add its unvisited, unblocked neighbours. Cells are
   stored in the queue as y * SNAKE_COLUMNS + x. The count does not include
   (x, y) itself. Each cell is queued at most once, so demo_queue[SNAKE_CELLS]
   cannot overflow; `limit` only keeps the search short when there is plenty
   of room. */
static unsigned open_space(const struct snake_game *game, int x, int y,
                           int eating, unsigned limit)
{
   for (int row = 0; row < SNAKE_ROWS; ++row)
      for (int column = 0; column < SNAKE_COLUMNS; ++column)
         demo_blocked[row][column] = 0;
   unsigned body = game->length - (eating ? 0u : 1u);
   for (unsigned segment = 0; segment < body; ++segment)
      demo_blocked[game->body[segment].y][game->body[segment].x] = 1;
   demo_blocked[y][x] = 1;
   unsigned head = 0;
   unsigned tail = 0;
   demo_queue[tail++] = (uint16_t)(y * SNAKE_COLUMNS + x);
   unsigned count = 0;
   while (head < tail && count < limit)
   {
      int cell = demo_queue[head++];
      int cx = cell % SNAKE_COLUMNS;
      int cy = cell / SNAKE_COLUMNS;
      for (int direction = 0; direction < 4; ++direction)
      {
         int nx = cx;
         int ny = cy;
         step_point((enum snake_direction)direction, &nx, &ny);
         if (!inside(nx, ny) || demo_blocked[ny][nx])
            continue;
         demo_blocked[ny][nx] = 1;
         demo_queue[tail++] = (uint16_t)(ny * SNAKE_COLUMNS + nx);
         ++count;
      }
   }
   return count;
}

/* The attract-mode autopilot. It looks one move ahead: for each direction
   that is not a reversal and does not crash immediately, it flood-fills to
   see how much room the snake would have there. A move is "safe" if at
   least length + 2 cells stay reachable (enough room for the whole body to
   follow without being boxed in). Choices are ranked by: safe first, then
   closest (Manhattan distance) to the target, then most room. The target is
   the food, or the mouse if one is within 7 cells. This greedy rule plays a
   convincing demo but is not perfect - it can still trap itself late in a
   long game, which is fine for attract mode. If every direction crashes it
   keeps going straight (and the demo ends). */
enum snake_direction snake_demo_direction(const struct snake_game *game)
{
   int hx = game->body[0].x;
   int hy = game->body[0].y;
   int tx = game->food.x;
   int ty = game->food.y;
   if (game->mouse_active)
   {
      int dx = game->mouse.x - hx;
      int dy = game->mouse.y - hy;
      if ((dx < 0 ? -dx : dx) + (dy < 0 ? -dy : dy) < 7)
      {
         tx = game->mouse.x;
         ty = game->mouse.y;
      }
   }
   enum snake_direction best = game->direction;
   int best_safe = -1;
   int best_distance = 0;
   unsigned best_space = 0;
   for (int candidate = 0; candidate < 4; ++candidate)
   {
      enum snake_direction direction = (enum snake_direction)candidate;
      if (((unsigned)direction ^ (unsigned)game->direction) == 2u)
         continue;
      int x = hx;
      int y = hy;
      step_point(direction, &x, &y);
      int eating = x == game->food.x && y == game->food.y;
      if (!inside(x, y) ||
          occupied(game, x, y, game->length - (eating ? 0u : 1u)))
         continue;
      unsigned space = open_space(game, x, y, eating, game->length + 8);
      int safe = space >= game->length + 2;
      int distance = (x > tx ? x - tx : tx - x) + (y > ty ? y - ty : ty - y);
      if (safe > best_safe ||
          (safe == best_safe && (distance < best_distance ||
                                 (distance == best_distance && space > best_space))))
      {
         best = direction;
         best_safe = safe;
         best_distance = distance;
         best_space = space;
      }
   }
   return best;
}
