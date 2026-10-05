/*
 * sign_test.c - long autopiloted games checking the SLOW sign and speed.
 *
 * The snake follows a fixed zig-zag path that covers the whole board (so it
 * never dies and eventually fills the board). Along the way we check that:
 *   - signs never appear at the starting speed (there is nothing to slow);
 *   - a sign never sits on the food or under the snake;
 *   - an uncollected sign disappears after at most 70 moves;
 *   - the speed level follows the documented rule: +1 every 5 foods (max 4),
 *     and eating a sign drops a level and restarts the count;
 *   - signs land on both "colours" of the checkerboard (a weak random number
 *     generator once limited them to half the squares).
 */
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include "snake_game.h"

/* The covering path: up column 0, then snake left and right along the rows. */
static enum snake_direction path(int x, int y)
{
   if (x == 0)
      return y == 0 ? SNAKE_RIGHT : SNAKE_UP;
   if (y % 2 == 0)
      return x < SNAKE_COLUMNS - 1 ? SNAKE_RIGHT : SNAKE_DOWN;
   if (x > 1)
      return SNAKE_LEFT;
   return y == SNAKE_ROWS - 1 ? SNAKE_LEFT : SNAKE_DOWN;
}

static void fail(const char *message, int run, long step)
{
   printf("FAIL run %d step %ld: %s\n", run, step, message);
   exit(1);
}

int main(void)
{
   struct snake_game game;
   long appeared = 0, eaten = 0, longest_life = 0, parity[2] = {0, 0};
   for (int run = 0; run < 5; run++)
   {
      snake_init(&game, (uint32_t)(run + 101));
      snake_start(&game);
      /* Put the snake on the path: column 0, heading up. */
      for (unsigned i = 0; i < game.length; i++)
      {
         game.body[i].x = 0;
         game.body[i].y = (int8_t)(10 + i);
      }
      game.direction = game.pending_direction = SNAKE_UP;
      unsigned model_level = 0, model_count = 0;
      long life = 0;
      for (long step = 0; step < 2000000; step++)
      {
         game.hawk_countdown = 1000000;   /* no hawk in this test */
         enum snake_direction want = path(game.body[0].x, game.body[0].y);
         if (want != game.pending_direction)
            snake_steer(&game, want);
         unsigned level_before = game.speed_level;
         enum snake_event event = snake_step(&game);
         if (event == SNAKE_EVENT_DIE)
            fail("the path should never die", run, step);
         if (event == SNAKE_EVENT_EAT || event == SNAKE_EVENT_WIN)
            if (++model_count >= 5) { model_count = 0; if (model_level < 4) model_level++; }
         if (game.sign_event == SNAKE_SIGN_EATEN)
         {
            eaten++;
            if (model_level) model_level--;
            model_count = 0;
         }
         if (game.sign_event == SNAKE_SIGN_APPEARED)
         {
            appeared++;
            life = 0;
            parity[(game.sign.x + game.sign.y) & 1]++;
            if (level_before == 0)
               fail("sign at the starting speed", run, step);
         }
         if (game.sign_active)
         {
            if (++life > longest_life) longest_life = life;
            if (life > 70) fail("sign stayed too long", run, step);
            if (game.sign.x == game.food.x && game.sign.y == game.food.y)
               fail("sign on the food", run, step);
            for (unsigned s = 0; s < game.length; s++)
               if (game.body[s].x == game.sign.x && game.body[s].y == game.sign.y)
                  fail("sign under the snake", run, step);
         }
         if (game.speed_level != model_level || game.foods_toward_level != model_count)
            fail("speed level does not follow the rules", run, step);
         if (event == SNAKE_EVENT_WIN)
            break;
      }
   }
   printf("signs appeared %ld, eaten %ld, longest life %ld moves; squares even %ld odd %ld\n",
          appeared, eaten, longest_life, parity[0], parity[1]);
   if (parity[0] * 3 < appeared || parity[1] * 3 < appeared)
      fail("signs favour half the board", 0, 0);
   printf("sign_test ok\n");
   return 0;
}
