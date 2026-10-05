/*
 * hawk_test.c - scenario test for the hawk attack.
 *
 * Puts a snake in open space, makes the hawk appear at once, and plays 5,000
 * seeded scenarios twice:
 *   strategy 0: keep going straight when the hawk dives;
 *   strategy 1: turn as soon as the dive starts.
 * While the hawk circles, the snake loops in a small circle to stay safe.
 *
 * Checked on every scenario:
 *   - the hawk always circles before it dives;
 *   - a catch kills the snake, happens exactly hawk_dive_steps after the dive
 *     starts, and only when the head is inside the 3x3 target zone;
 *   - a miss only happens when the head is outside the zone.
 * The game design promise is also checked: turning at once ALWAYS escapes
 * (a dodge should be a skill, not luck), and going straight is risky.
 */
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include "snake_game.h"

enum outcome { DODGED, CAUGHT, CRASHED, NOTHING };

static void fail(const char *message, int seed, int step)
{
   printf("FAIL seed %d step %d: %s\n", seed, step, message);
   exit(1);
}

static enum outcome scenario(int seed, int strategy)
{
   struct snake_game game;
   snake_init(&game, (uint32_t)seed);
   snake_start(&game);
   game.hawk_countdown = 0;               /* hawk now */
   game.speed_level = (unsigned)(rand() % 5);
   game.mouse_countdown = 100000;         /* no mouse or sign to interfere */
   game.sign_countdown = 100000;

   /* A random straight snake away from the walls. */
   int x = 3 + rand() % 12, y = 3 + rand() % 22;
   enum snake_direction direction = (enum snake_direction)(rand() % 4);
   int back_x = direction == SNAKE_RIGHT ? -1 : direction == SNAKE_LEFT ? 1 : 0;
   int back_y = direction == SNAKE_DOWN ? -1 : direction == SNAKE_UP ? 1 : 0;
   for (unsigned i = 0; i < game.length; i++)
   {
      game.body[i].x = (int8_t)(x + back_x * (int)i);
      game.body[i].y = (int8_t)(y + back_y * (int)i);
   }
   game.direction = game.pending_direction = direction;
   game.food.x = 0;
   game.food.y = 0;

   int arrived = 0, dive_step = 0;
   bool turned = false;
   for (int step = 0; step < 120; step++)
   {
      if (game.hawk_phase != SNAKE_HAWK_DIVING && step % 3 == 0)
         snake_steer(&game, (enum snake_direction)((game.pending_direction + 1) % 4));
      if (game.hawk_phase == SNAKE_HAWK_DIVING && strategy == 1 && !turned)
      {
         snake_steer(&game, (enum snake_direction)((game.direction + 1) % 4));
         turned = true;
      }
      enum snake_event event = snake_step(&game);
      if (game.hawk_event == SNAKE_HAWK_ARRIVED)
         arrived = step ? step : 1;
      if (game.hawk_event == SNAKE_HAWK_DIVES)
      {
         dive_step = step;
         if (!arrived)
            fail("dived without circling first", seed, step);
      }
      int dx = game.body[0].x - game.hawk_target.x;
      int dy = game.body[0].y - game.hawk_target.y;
      bool in_zone = abs(dx) <= 1 && abs(dy) <= 1;
      if (game.hawk_event == SNAKE_HAWK_CAUGHT)
      {
         if (event != SNAKE_EVENT_DIE)
            fail("caught but the game goes on", seed, step);
         if (!in_zone)
            fail("caught outside the target zone", seed, step);
         if ((unsigned)(step - dive_step) != game.hawk_dive_steps)
            fail("strike came at the wrong time", seed, step);
         return CAUGHT;
      }
      if (game.hawk_event == SNAKE_HAWK_MISSED)
      {
         if (in_zone)
            fail("missed although the head was in the zone", seed, step);
         return DODGED;
      }
      if (event == SNAKE_EVENT_DIE)
         return CRASHED;
   }
   return NOTHING;
}

int main(void)
{
   int results[2][4] = {{0}};
   for (int seed = 1; seed <= 5000; seed++)
   {
      srand((unsigned)seed);
      results[0][scenario(seed, 0)]++;
      srand((unsigned)seed);
      results[1][scenario(seed, 1)]++;
   }
   printf("keep straight: caught %d, dodged %d, crashed %d\n",
          results[0][CAUGHT], results[0][DODGED], results[0][CRASHED]);
   printf("turn at once:  caught %d, dodged %d, crashed %d\n",
          results[1][CAUGHT], results[1][DODGED], results[1][CRASHED]);
   if (results[1][CAUGHT] != 0)
      fail("turning at once should always escape", 0, 0);
   if (results[0][CAUGHT] == 0)
      fail("going straight should get caught", 0, 0);
   printf("hawk_test ok\n");
   return 0;
}
