/*
 * frontend_test.c - runs the whole libretro core inside a fake Retroplayer.
 *
 * This file #includes snake_libretro.c directly, so it can call the core's
 * entry points AND peek at its private state (attract, game, settings_open).
 * It is compiled with -DALP_SNAKE_HOST_PREVIEW, which makes every system call
 * fail harmlessly: no files are written, no backglass is started, and with no
 * clock the core falls back to counting frames, which makes the run
 * repeatable. That lets it run on a Mac or PC.
 *
 * The fake frontend answers environment calls the way the cabinet does
 * (including the Adv. Config variable), feeds button presses, counts audio,
 * and saves screenshots as PPM images in tests/out/.
 *
 * Checked:
 *   1. launch goes to attract mode with music, and the demo snake plays;
 *   2. Insert Coin (Select) starts a real game;
 *   3. Adv. Config opens the DIP switch page and pauses the game, switches
 *      can be flipped, and closing the cabinet menu (which also calls
 *      retro_reset) closes the page without losing the game;
 *   4. the game runs into a wall, ends, and returns to attract mode;
 *   5. about one frame of audio is produced per frame.
 */
#include "snake_libretro.c"
#include <stdio.h>
#include <string.h>

static int failures;
#define CHECK(condition, message) do { \
   printf("%s %s\n", (condition) ? "  ok  " : "  FAIL", message); \
   if (!(condition)) failures++; } while (0)

/* ---- the fake frontend ---- */
static const char *display_setup = "disabled";   /* Adv. Config variable */
static bool variable_updated;
static int16_t held[16];                           /* joypad buttons held */
static unsigned long audio_frames;

static bool environment(unsigned command, void *data)
{
   switch (command)
   {
   case RETRO_ENVIRONMENT_SET_PIXEL_FORMAT:
   case RETRO_ENVIRONMENT_SET_VARIABLES:
   case RETRO_ENVIRONMENT_SET_INPUT_DESCRIPTORS:
   case RETRO_ENVIRONMENT_SET_CONTROLLER_INFO:
      return true;
   case RETRO_ENVIRONMENT_GET_VARIABLE:
   {
      /* Like the cabinet: unknown variables get a default, never a failure. */
      struct retro_variable *variable = data;
      variable->value = strcmp(variable->key, "mame2003-plus_display_setup") == 0
                        ? display_setup : "disabled";
      return true;
   }
   case RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE:
      *(bool *)data = variable_updated;
      variable_updated = false;
      return true;
   default:
      return false;
   }
}
static int16_t input_state(unsigned port, unsigned device, unsigned index, unsigned id)
{
   (void)index;
   return port == 0 && device == RETRO_DEVICE_JOYPAD && id < 16 ? held[id] : 0;
}
static void input_poll(void) {}
static size_t audio_batch(const int16_t *data, size_t frames)
{
   (void)data;
   audio_frames += frames;
   return frames;
}

static void run_frames(int count) { for (int i = 0; i < count; i++) retro_run(); }
static void tap(unsigned button)
{
   held[button] = 1; run_frames(3);
   held[button] = 0; run_frames(4);
}
/* What Retroplayer does for Adv. Config and for closing its menu (seen with
   the probe build): flip the variable, announce it, tap R3 for 4 frames. */
static void adv_config_open(void)
{
   display_setup = "enabled"; variable_updated = true; run_frames(1);
   held[RETRO_DEVICE_ID_JOYPAD_R3] = 1; run_frames(4); held[RETRO_DEVICE_ID_JOYPAD_R3] = 0;
   run_frames(4);
}
static void adv_config_close(void)
{
   retro_reset();
   display_setup = "disabled"; variable_updated = true; run_frames(12);
   held[RETRO_DEVICE_ID_JOYPAD_R3] = 1; run_frames(4); held[RETRO_DEVICE_ID_JOYPAD_R3] = 0;
   run_frames(4);
}

/* Saves the frame the core last presented as a binary PPM (RGB565 -> RGB). */
static void screenshot(const char *name)
{
   char path[256];
   snprintf(path, sizeof(path), "tests/out/%s.ppm", name);
   FILE *file = fopen(path, "wb");
   if (!file) { printf("  (could not write %s)\n", path); return; }
   fprintf(file, "P6\n%d %d\n255\n", SCREEN_WIDTH, SCREEN_HEIGHT);
   const uint16_t *pixels = frame_buffers[front_buffer];
   for (int i = 0; i < SCREEN_WIDTH * SCREEN_HEIGHT; i++)
   {
      uint16_t p = pixels[i];
      unsigned char rgb[3] = {
         (unsigned char)((p >> 11) * 255 / 31),
         (unsigned char)(((p >> 5) & 63) * 255 / 63),
         (unsigned char)((p & 31) * 255 / 31) };
      fwrite(rgb, 1, 3, file);
   }
   fclose(file);
   printf("  wrote %s\n", path);
}

int main(void)
{
   retro_set_environment(environment);
   retro_set_input_state(input_state);
   retro_set_input_poll(input_poll);
   retro_set_audio_sample_batch(audio_batch);
   struct retro_game_info content = { "./roms/snake.zip", 0, 0, 0 };
   retro_load_game(&content);

   printf("1. launch and attract mode\n");
   CHECK(attract, "starts in attract mode");
   CHECK(snake_audio_music_playing(), "attract music is playing");
   run_frames(240);
   CHECK(game.steps > 10, "the demo snake is playing by itself");
   screenshot("attract_title");
   run_frames(ATTRACT_PANEL_FRAMES);
   screenshot("attract_scores");

   printf("2. Insert Coin\n");
   tap(RETRO_DEVICE_ID_JOYPAD_SELECT);
   CHECK(!attract && game.phase == SNAKE_PLAYING, "Select (Insert Coin) starts a game");
   game.hawk_countdown = 100000;    /* keep this test predictable */
   run_frames(40);   /* a few moves; the snake heads for the top wall */
   screenshot("playing");

   printf("3. Adv. Config / DIP switches\n");
   unsigned steps_before = game.steps;
   adv_config_open();
   CHECK(settings_open, "Adv. Config opens the DIP switch page");
   CHECK(game.phase == SNAKE_PAUSED, "the game is paused behind it");
   screenshot("dip_switches");
   tap(RETRO_DEVICE_ID_JOYPAD_RIGHT);            /* first switch: hawk */
   CHECK(!setting_on(SETTING_HAWK), "a flipper/stick press turns the hawk off");
   tap(RETRO_DEVICE_ID_JOYPAD_RIGHT);
   CHECK(setting_on(SETTING_HAWK), "and back on");
   adv_config_close();
   CHECK(!settings_open, "closing the cabinet menu closes the page");
   CHECK(game.phase == SNAKE_PLAYING && game.steps >= steps_before,
         "the reset that comes with it did not throw the game away");

   printf("4. game over and back to attract\n");
   int frames = 0;
   while (game.phase == SNAKE_PLAYING && frames < 20000) { retro_run(); frames++; }
   CHECK(game.phase == SNAKE_GAME_OVER, "running straight ends the game");
   screenshot("game_over");
   if (entering_name)                      /* a top-ten score asks for initials */
      tap(RETRO_DEVICE_ID_JOYPAD_START);
   frames = 0;
   while (!attract && frames < 3000) { retro_run(); frames++; }
   CHECK(attract, "returns to attract mode by itself");

   printf("5. audio\n");
   unsigned long expected = (unsigned long)frame_count * AUDIO_FRAMES;
   CHECK(audio_frames > expected * 9 / 10 && audio_frames < expected * 11 / 10,
         "about 735 samples per frame were produced");

   retro_unload_game();
   printf(failures ? "frontend_test FAILED (%d)\n" : "frontend_test ok\n", failures);
   return failures ? 1 : 0;
}
