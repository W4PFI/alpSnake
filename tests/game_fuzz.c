/*
 * game_fuzz.c - random games against the rules engine, checking invariants.
 *
 * snake_game.c knows nothing about the cabinet, so it compiles and runs on
 * any computer. This test plays 20,000 games with a "mostly sensible, often
 * random" player and checks after EVERY move that:
 *   - the body is one connected chain with no gaps and never overlaps itself;
 *   - the length and pending shrink match an independent model of the rules
 *     (+1 per food; a caught mouse removes 3 segments over the next moves,
 *     never going below the starting length of 4);
 *   - the score is exactly 10 per food plus 20 per mouse;
 *   - the mouse is never on the snake, and food is never under the body.
 * Any failure prints the run and step and exits with status 1.
 *
 * Build and run: tests/run_tests.sh (or see the command at the bottom).
 */
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include "snake_game.h"

static void fail(const char *message, int run, int step)
{
   printf("FAIL run %d step %d: %s\n", run, step, message);
   exit(1);
}

int main(void)
{
   struct snake_game game;
   long steps = 0, mice_caught = 0;
   srand(1);
   for (int run = 0; run < 20000; run++)
   {
      snake_init(&game, (uint32_t)(run * 7 + 1));
      snake_start(&game);
      unsigned foods = 0, mice = 0;
      unsigned model_length = 4, model_shrink = 0;   /* our own bookkeeping */
      for (int step = 0; step < 30000; step++)
      {
         /* Head for the mouse if there is one, else the food; 3 times in 4
            steer towards it, sometimes steer somewhere random. */
         int target_x = game.mouse_active ? game.mouse.x : game.food.x;
         int target_y = game.mouse_active ? game.mouse.y : game.food.y;
         int head_x = game.body[0].x, head_y = game.body[0].y;
         enum snake_direction want = target_x > head_x ? SNAKE_RIGHT
                                   : target_x < head_x ? SNAKE_LEFT
                                   : target_y > head_y ? SNAKE_DOWN : SNAKE_UP;
         if (rand() % 4)
            snake_steer(&game, want);
         else if (rand() % 3 == 0)
            snake_steer(&game, (enum snake_direction)(rand() % 4));

         enum snake_event event = snake_step(&game);
         steps++;
         if (event == SNAKE_EVENT_DIE)
            break;

         /* Update the independent model of length and score. */
         bool ate = event == SNAKE_EVENT_EAT || event == SNAKE_EVENT_WIN;
         if (ate) { foods++; model_length++; }
         else if (model_shrink) { if (model_length > 4) model_length--; model_shrink--; }
         if (game.mouse_event == SNAKE_MOUSE_CAUGHT) { mice++; mice_caught++; model_shrink += 3; }

         if (game.length != model_length || game.shrink_pending != model_shrink)
            fail("length model mismatch", run, step);
         if (game.length < 4)
            fail("snake shorter than it starts", run, step);
         if (game.score != foods * 10 + mice * 20)
            fail("score mismatch", run, step);
         for (unsigned s = 0; s < game.length; s++)
         {
            if (s)
            {
               int gap = abs(game.body[s].x - game.body[s - 1].x) +
                         abs(game.body[s].y - game.body[s - 1].y);
               if (gap != 1)
                  fail("gap in the body", run, step);
            }
            for (unsigned t = s + 1; t < game.length; t++)
               if (game.body[s].x == game.body[t].x && game.body[s].y == game.body[t].y)
                  fail("body overlaps itself", run, step);
            if (game.mouse_active && game.body[s].x == game.mouse.x &&
                game.body[s].y == game.mouse.y)
               fail("mouse on the snake", run, step);
            if (s > 0 && game.body[s].x == game.food.x && game.body[s].y == game.food.y)
               fail("food under the body", run, step);
         }
         if (event == SNAKE_EVENT_WIN)
            break;
      }
   }
   printf("game_fuzz ok: %ld moves in 20000 games, %ld mice caught\n", steps, mice_caught);
   return 0;
}
