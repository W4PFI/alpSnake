/* ==========================================================================
   snake_libretro.c - the ALP Snake libretro core
   ==========================================================================

   This is the part of ALP Snake that the AtGames Legends Pinball cabinet
   actually loads. The cabinet's game launcher, Retroplayer, is a libretro
   frontend: it dlopen()s a "core" (a shared library exporting the retro_*
   functions declared in libretro.h), hands it callbacks for video, audio
   and input, and then calls retro_run() once per display frame. This file
   is that core. The game rules themselves live in snake_game.c and know
   nothing about the hardware; the synthesizer lives in snake_audio.c. This
   file glues them to the cabinet: it draws every pixel, feeds sound, reads
   the controls, saves high scores and settings, and starts the separate
   backglass program.

   The libretro lifecycle, in the order Retroplayer calls things:
     retro_set_environment()      gets the "environment" callback used to
                                  ask the frontend for things (variables,
                                  pixel format, ...).
     retro_set_video_refresh() etc. receive the other callbacks.
     retro_init()                 nothing to do here.
     retro_get_system_info()      we claim to be MAME 2003-Plus (see below).
     retro_load_game()            the real start-up: pixel format, core
                                  variables, settings, high scores, attract
                                  mode, backglass launch, first frame.
     retro_get_system_av_info()   1080x1920 portrait, 60 fps, 44.1 kHz.
     retro_run()  (every frame)   options -> input -> game step -> draw ->
                                  present -> audio -> keepalive.
     retro_reset()                sent when the cabinet menu closes; mostly
                                  ignored (see there).
     retro_unload_game()          save, stop the backglass, close devices.
     retro_deinit()

   Why pretend to be MAME 2003-Plus? Retroplayer only offers its full arcade
   menu (Insert Coin, Save Slots, Adv. Config, trackball resolution) to that
   core, so retro_get_system_info() and the core variables below copy it
   word for word. Insert Coin presses Select (we start a game); Adv. Config
   sets the variable mame2003-plus_display_setup to "enabled" and taps R3,
   which we use to open our own DIP switch settings page.

   Freestanding build: the core is compiled with -nostdlib -ffreestanding
   -fno-builtin, so there is no libc at all. Files, clocks, processes and
   input devices are reached with raw Linux system calls (system_call()
   below, numbers in alp_syscall.h). Large structures are never copied or
   zero-initialised with "= {0}", because the compiler would turn that into
   calls to memcpy/memset, which do not exist; static zeroed arrays and
   small hand-written loops are used instead.

   Map of the file:
     1. Constants and global state (screen, audio, settings, attract mode,
        high-score table, confetti).
     2. system_call(): the aarch64 "svc #0" wrapper.
     3. Probe build helpers (ALP_SNAKE_PROBE only): log Retroplayer's calls.
     4. Drawing primitives: rectangles, bitmap text, anti-aliased text.
     5. Small game helpers: timing, name entry, steering.
     6. Confetti.
     7. High-score table: ranking, name entry, load/save with migration.
     8. Backglass: state file, launching and stopping the helper program.
     9. Trackball: finding and reading /dev/input/event* directly.
    10. Attract mode and the DIP switch page's input.
    11. poll_controls(): all input for one frame.
    12. Sprites and screens: head, tail, mouse, sign, hawk, panels,
        draw_game().
    13. Audio clock, frame-rate diagnostics, the SDL keepalive.
    14. Settings persistence and the cabinet-menu (Adv. Config) handshake.
    15. Probe-build checks of variables and input.
    16. The libretro entry points, including retro_run().

   Key ideas for a learner:
     - Only redraw when something changed (frame_dirty); otherwise hand the
       frontend the same finished buffer again, which is nearly free.
     - Drive game speed and audio from the monotonic clock, not from a frame
       count, because the cabinet's frames are not perfectly regular.
     - Write every file atomically (temp file, fsync, rename) so a power cut
       can never leave a half-written save.
   ========================================================================== */
#include "alp_syscall.h"
#include "libretro.h"
#include "snake_audio.h"
#include "snake_backglass_state.h"
#include "snake_font.h"
#include "snake_game.h"
#include "snake_smooth_font.h"

#include <stdbool.h>
#include <stdint.h>

#if !defined(__aarch64__) && !defined(ALP_SNAKE_HOST_PREVIEW)
#error This core targets AArch64 Linux
#endif

/* ---- 1. Constants and global state ------------------------------------ */

/* The cabinet's playfield monitor is a portrait 1080x1920 panel. */
#define SCREEN_WIDTH 1080
#define SCREEN_HEIGHT 1920
/* 44100 Hz / 60 fps: the nominal number of stereo samples per frame, used
   when no clock reading is available or after a pause. */
#define AUDIO_FRAMES 735
/* Up to a quarter-second of sound can be made up after a slow frame. */
#define AUDIO_MAX_FRAMES 11025
/* The board is SNAKE_COLUMNS x SNAKE_ROWS (18 x 28) cells of 54 pixels,
   972 x 1512 pixels, with its top-left corner here. The header with the
   title and scores sits above it. */
#define BOARD_X 54
#define BOARD_Y 249
#define CELL_SIZE 54
#define HIGH_SCORE_PATH "./save/hiscore.dat"
#define HIGH_SCORE_TEMP_PATH "./save/hiscore.tmp"
/* File magics are ASCII tags read as little-endian words: "SNKH" was the
   original best-score file, "SNK2" the version that added initials. Both
   are only read now, to migrate into the top-ten table ("SNK3"). */
#define HIGH_SCORE_MAGIC 0x534e4b48u
#define HIGH_SCORE_NAME_MAGIC 0x534e4b32u
#define NAME_LENGTH 3

/* The only SDL function the core uses (see keepalive()). It resolves at
   run time to the cabinet's own libSDL2, which Retroplayer has already
   loaded; at build time tools/sdl2_link_stub.c provides a link-only stub.
   The host preview build has no SDL at all, so it gets a dummy. */
extern int SDL_PushEvent(void *event);
#ifdef ALP_SNAKE_HOST_PREVIEW
int SDL_PushEvent(void *event) { (void)event; return 1; }
#endif

/* Callbacks handed to us by Retroplayer through the retro_set_* functions. */
static retro_environment_t environment_callback;
static retro_video_refresh_t video_callback;
static retro_audio_sample_t audio_callback;
static retro_audio_sample_batch_t audio_batch_callback;
static retro_input_poll_t input_poll_callback;
static retro_input_state_t input_state_callback;
/* Two RGB565 frame buffers (about 4 MB each). draw_game() paints the back
   one (drawing_frame), then flips front_buffer; retro_run() always presents
   frame_buffers[front_buffer]. Static, so they start zeroed without memset. */
static uint16_t frame_buffers[2][SCREEN_WIDTH * SCREEN_HEIGHT];
static uint16_t *drawing_frame;
static unsigned front_buffer;
/* Interleaved stereo samples (left, right) for one retro_run. Each frame
   produces however much real time has passed, so this holds up to
   AUDIO_MAX_FRAMES stereo frames: a quarter second of sound. */
static int16_t audio[AUDIO_MAX_FRAMES * 2];
/* When audio was last generated, and the leftover fraction of a sample
   (in units of 1/1e9 sample) so rounding never drifts. */
static uint64_t audio_clock_ns;
static uint64_t audio_remainder;
/* Diagnostics, shown while both flippers are held on the attract, Ready or
   Game Over screen: measured frame rate and how much sound the frontend
   accepted. */
static uint64_t diagnostic_window_ns;
static unsigned diagnostic_frames;
static unsigned long diagnostic_offered;
static unsigned long diagnostic_accepted;
static unsigned measured_fps_x100;
/* Slowest frame interval and slowest drawing in the last window, in ms. */
static unsigned worst_gap_ms;
static unsigned worst_draw_ms;
static unsigned shown_gap_ms;
static unsigned shown_draw_ms;
static uint64_t previous_frame_ns;
static unsigned audio_accepted_percent = 100;
static bool show_diagnostics;

/* Settings offered to the cabinet's options menu. Retroplayer lists a core's
   libretro variables there; this matches what the cabinet's MAME 2003-Plus
   core does (original "Name; value|value" variables, sent while the game
   loads), which is what shows up as the DIP switch menu for MAME games. */
static const struct retro_variable option_variables[] = {
   /* The cabinet's MAME 2003-Plus core settings, word for word. Presenting
      as that core is what makes Retroplayer offer its full menu (Insert
      Coin, Save Slots, Adv. Config). Adv. Config switches
      mame2003-plus_display_setup to "enabled", which opens our DIP switch
      page. */
   { "mame2003-plus_four_way_emulation", "4-way joystick emulation on 8-way joysticks; disabled|enabled" },
   { "mame2003-plus_mouse_device", "Mouse Device; mouse|pointer|disabled" },
   { "mame2003-plus_crosshair_enabled", "Show Lightgun crosshair; enabled|disabled" },
   { "mame2003-plus_skip_disclaimer", "Skip Disclaimer; disabled|enabled" },
   { "mame2003-plus_skip_warnings", "Skip Warnings; disabled|enabled" },
   { "mame2003-plus_display_setup", "Display MAME menu; disabled|enabled" },
   { "mame2003-plus_brightness", "Brightness; 1.0|0.2|0.3|0.4|0.5|0.6|0.7|0.8|0.9|1.1|1.2|1.3|1.4|1.5|1.6|1.7|1.8|1.9|2.0" },
   { "mame2003-plus_gamma", "Gamma correction; 1.0|0.5|0.6|0.7|0.8|0.9|1.1|1.2|1.3|1.4|1.5|1.6|1.7|1.8|1.9|2.0" },
   { "mame2003-plus_display_artwork", "Display artwork (Restart core); enabled|disabled" },
   { "mame2003-plus_art_resolution", "Artwork resolution multiplier (Restart core); 1|2" },
   { "mame2003-plus_neogeo_bios", "Specify Neo Geo BIOS (Restart core); default|euro|euro-s1|us|us-e|asia|japan|japan-s2|unibios33|unibios20|unibios13|unibios11|unibios10|debug|asia-aes" },
   { "mame2003-plus_stv_bios", "Specify Sega ST-V BIOS (Restart core); default|japan|japana|us|japan_b|taiwan|europe" },
   { "mame2003-plus_use_alt_sound", "Use CD soundtrack (Restart core); enabled|disabled" },
   { "mame2003-plus_dialsharexy", "Share 2 player dial controls across one X/Y device; disabled|enabled" },
   { "mame2003-plus_analog", "Control mapping ; analog|digital" },
   { "mame2003-plus_deadzone", "Analog deadzone; 20|0|5|10|15|25|30|35|40|45|50|55|60|65|70|75|80|85|90|95" },
   { "mame2003-plus_tate_mode", "TATE Mode - Rotating display (Restart core); disabled|enabled" },
   { "mame2003-plus_vector_resolution", "Vector resolution (Restart core); 1024x768|640x480|1280x960|1440x1080|1600x1200|original" },
   { "mame2003-plus_vector_antialias", "Vector antialiasing; enabled|disabled" },
   { "mame2003-plus_vector_beam_width", "Vector beam width (only with antialiasing); 2|1|1.2|1.4|1.6|1.8|2.5|3|4|5|6|7|8|9|10|11|12" },
   { "mame2003-plus_vector_translucency", "Vector translucency; enabled|disabled" },
   { "mame2003-plus_vector_flicker", "Vector flicker; 20|0|10|30|40|50|60|70|80|90|100" },
   { "mame2003-plus_vector_intensity", "Vector intensity; 1.5|0.5|1|2|2.5|3" },
   { "mame2003-plus_nvram_bootstraps", "NVRAM Bootstraps; enabled|disabled" },
   { "mame2003-plus_sample_rate", "Sample Rate (KHz); 48000|8000|11025|22050|30000|44100|" },
   { "mame2003-plus_dcs_speedhack", "DCS Speedhack; enabled|disabled" },
   { "mame2003-plus_input_interface", "Input interface; retropad|keyboard|simultaneous" },
   { "mame2003-plus_mame_remapping", "Legacy Remapping (restart); enabled|disabled" },
   { "mame2003-plus_frameskip", "Frameskip; 0|1|2|3|4|5" },
   { "mame2003-plus_core_sys_subfolder", "Locate system files within a subfolder; enabled|disabled" },
   { "mame2003-plus_core_save_subfolder", "Locate save files within a subfolder; enabled|disabled" },
   { "mame2003-plus_cheat_input_ports", "Dip switch/Cheat input ports; disabled|enabled" },
   { "mame2003-plus_machine_timing", "Bypass audio skew (Restart core); enabled|disabled" },
   { 0, 0 }
};
#define DISPLAY_SETUP_VARIABLE 5   /* index of mame2003-plus_display_setup */
/* True once Retroplayer has answered a GET_VARIABLE (shown in diagnostics). */
static bool options_from_cabinet;
static bool move_sound_enabled = true;
static bool attract_music_enabled = true;
static unsigned configured_features = SNAKE_FEATURES_ALL;

/* The DIP switch page, opened from the cabinet menu's Adv. Config. The
   cabinet never shows core options, so the game keeps its own settings as
   one bit per switch (bit n = SETTING_n, 1 = on) in save/settings.dat. */
#define SETTING_HAWK 0
#define SETTING_MOUSE 1
#define SETTING_SIGNS 2
#define SETTING_MOVE_SOUND 3
#define SETTING_MUSIC 4
#define SETTING_COUNT 5
#define SETTINGS_PATH "./save/settings.dat"
#define SETTINGS_TEMP_PATH "./save/settings.tmp"
#define SETTINGS_MAGIC 0x534e4b4fu   /* "SNKO" */
static unsigned settings_bits = (1u << SETTING_COUNT) - 1;   /* all on */
static bool settings_open;
static bool settings_paused_game;
static unsigned settings_cursor;          /* 0..SETTING_COUNT; the last is EXIT */
static unsigned settings_closed_frame;    /* frame_count when the page closed */
static bool display_setup_enabled;        /* last seen "Display MAME menu" value */
static void open_settings(void);
static void close_settings(void);
static void apply_settings(void);
static bool setting_on(unsigned setting);

/* Attract mode: a computer-played demo game with music and rotating panels,
   shown at launch and a few seconds after each game ends. */
#define ATTRACT_DEMO_PERIOD 6        /* frames per demo move: 10 moves/s */
#define ATTRACT_DEMO_MOVES 450       /* restart the demo after ~45 s */
#define ATTRACT_DEMO_MAX_LENGTH 36   /* ...or once the snake gets long */
#define ATTRACT_PANEL_FRAMES 420     /* each panel shows for 7 s */
#define GAME_OVER_TO_ATTRACT 480     /* back to attract 8 s after a game */
static bool attract;
static unsigned attract_frames;
/* The demo game shares the `game` struct with real play, so the real best
   score is parked here while the demo runs. */
static unsigned attract_saved_best;
static unsigned demo_frames;
static unsigned demo_dead_frames;
static unsigned game_over_frames;
static unsigned last_score;
/* Text that pops up briefly where something was eaten: "+20" for a mouse,
   "SLOWER" for a slow sign. */
static bool popup_is_slow;
static const char *popup_points = "20";
/* Hawk visuals that outlive the game step: flying off after a miss, or
   standing over the snake after a catch. */
static unsigned hawk_flyoff_frames;
static bool hawk_has_snake;
/* After a catch the hawk lifts the snake off the board before the Game Over
   box appears; the snake is drawn this many pixels higher while it rises. */
#define HAWK_CARRY_FRAMES 90
static unsigned hawk_carry_frames;
static int snake_lift;
static unsigned catch_popup_frames;
static int catch_popup_x;
static int catch_popup_y;
/* The one game in progress (or the attract demo). Rules: snake_game.c. */
static struct snake_game game;
static unsigned frame_count;   /* retro_run calls so far; drives animations */
/* Movement timing. Normally the snake moves when the monotonic clock
   reaches next_move_ns; move_frames is a frame-counting fallback used only
   if the clock can't be read. last_run_ns spots pauses (see RESUME_GAP_NS). */
static unsigned move_frames;
static uint64_t next_move_ns;
static uint64_t last_run_ns;
/* Moves land on the frame nearest their deadline: half a 60 Hz frame. */
#define HALF_FRAME_NS 8333333u
/* A gap this long between frames means Retroplayer was paused or stalled. */
#define RESUME_GAP_NS 250000000u
/* Previous button states, so actions fire on the press, not while held. */
static bool start_was_pressed;
static bool left_was_pressed;
static bool right_was_pressed;
/* Set whenever the picture must change; retro_run redraws only then. */
static bool frame_dirty;
static unsigned head_animation;   /* see compute_head_animation() */
static bool high_score_dirty;     /* game.best rose during the current game */
static long trackball_descriptor = -1;   /* open /dev/input/eventN, or -1 */
static long backglass_process = -1;      /* pid of the backglass helper */
/* Trackball motion accumulated but not yet turned into a direction. */
static int trackball_x;
static int trackball_y;

/* The kernel's struct input_event as read from /dev/input/event* on 64-bit
   Linux: a struct timeval (two longs) followed by type, code and value,
   24 bytes in all. Declared here because there are no system headers. */
struct input_event {
   long seconds;
   long microseconds;
   uint16_t type;
   uint16_t code;
   int32_t value;
};

/* The old single-best-score file layout ("SNKH": 12 bytes, no name;
   "SNK2": with initials). Kept only for migration in load_high_score(). */
struct high_score_record {
   uint32_t magic;
   uint32_t best;
   uint32_t best_inverse;
   char name[4];
};

/* Letters offered for initials, in the order the stick cycles through them. */
static const char name_characters[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789 ";
#define NAME_CHARACTER_COUNT (sizeof(name_characters) - 1)
static char best_name[NAME_LENGTH + 1];   /* initials of the top score */

/* The top-ten table, best first. Empty places have score 0 and no name. */
#define HIGH_SCORE_COUNT 10
#define HIGH_SCORE_TABLE_MAGIC 0x534e4b33u
struct high_score_entry {
   uint32_t score;
   char name[4];
};
static struct high_score_entry high_scores[HIGH_SCORE_COUNT];
static int entry_rank = -1;       /* place being entered, or -1 */
static int newest_rank = -1;      /* last place filled, highlighted in attract */
static int last_game_rank = -1;   /* where the last game placed, or -1 */
static char last_initials[NAME_LENGTH + 1];   /* offered first next time */
static char entry_name[NAME_LENGTH + 1];      /* initials being spelled */
static unsigned entry_cursor;                 /* which of the 3 letters */
static bool entering_name;
static bool last_game_record;   /* last game took first place */
static unsigned best_at_start;
/* Auto-repeat for the stick on the name-entry and DIP switch pages: the
   direction held (-1 none) and for how many frames. */
static int held_direction = -1;
static unsigned held_frames;

/* Confetti, in 1/256-pixel fixed point (positions and velocities per
   frame), for a new number-one score. */
#define CONFETTI_COUNT 160
struct confetti {
   int32_t x;
   int32_t y;
   int32_t vx;
   int32_t vy;
   uint16_t color;
   uint8_t width;
   uint8_t height;
   uint8_t flip;
   bool live;
};
static struct confetti confetti[CONFETTI_COUNT];
static bool confetti_active;
static uint32_t confetti_random = 0x2545f491u;   /* any nonzero xorshift seed */

static void save_high_score(void);

/* ---- 2. System calls --------------------------------------------------- */

/* Makes a Linux system call directly, since there is no libc. On aarch64
   the call number goes in register x8, up to six arguments in x0-x5, and
   "svc #0" traps into the kernel; the result comes back in x0. Errors are
   returned as negative errno values (e.g. -4 EINTR, -19 ENODEV), not via
   errno. The "memory" clobber tells the compiler the kernel may read or
   write memory we passed pointers to. The host preview build (for testing
   drawing on a PC) has no real system calls and just fails every call. */
static long system_call(long number, long first, long second, long third,
                        long fourth, long fifth, long sixth)
{
#ifdef ALP_SNAKE_HOST_PREVIEW
   (void)number;
   (void)first;
   (void)second;
   (void)third;
   (void)fourth;
   (void)fifth;
   (void)sixth;
   return -1;
#else
   register long arg0 asm("x0") = first;
   register long arg1 asm("x1") = second;
   register long arg2 asm("x2") = third;
   register long arg3 asm("x3") = fourth;
   register long arg4 asm("x4") = fifth;
   register long arg5 asm("x5") = sixth;
   register long call_number asm("x8") = number;
   asm volatile("svc #0" : "+r"(arg0) : "r"(arg1), "r"(arg2), "r"(arg3),
                "r"(arg4), "r"(arg5), "r"(call_number) : "memory");
   return arg0;
#endif
}

#ifdef ALP_SNAKE_PROBE
/* ------------------------------------------------------------------------
   Probe build: ALP Snake presents itself to Retroplayer the way the
   cabinet's MAME 2003-Plus core does, and records everything Retroplayer
   does to it in ./save/probe.log (inside the UCE's save area), so we can see
   how the cabinet's MAME-only menu entries (Adv. Config, Save Slots, Insert
   Coin...) talk to a core.
   ------------------------------------------------------------------------ */
static long probe_fd = -1;
static unsigned probe_line_count;
static char probe_recent[5][44];
static unsigned probe_recent_next;
static bool probe_menu_requested;
static unsigned frame_count_for_probe(void) { return frame_count; }

/* One log line being assembled (there is no printf). */
struct probe_line {
   char text[160];
   unsigned length;
};

/* Appends a string, leaving room for the newline probe_finish() adds. */
static void probe_add(struct probe_line *line, const char *text)
{
   for (unsigned index = 0; text && text[index] && line->length < sizeof(line->text) - 2; ++index)
      line->text[line->length++] = text[index];
}

/* Appends a number in decimal: digits are produced backwards, then copied
   out in reverse. */
static void probe_number(struct probe_line *line, unsigned long value)
{
   char digits[24];
   unsigned count = 0;
   do
   {
      digits[count++] = (char)('0' + value % 10);
      value /= 10;
   } while (value && count < sizeof(digits));
   while (count && line->length < sizeof(line->text) - 2)
      line->text[line->length++] = digits[--count];
}

/* Starts a line with the frame number, e.g. "F123 retro_reset". */
static void probe_begin(struct probe_line *line, const char *what)
{
   line->length = 0;
   probe_add(line, "F");
   probe_number(line, frame_count_for_probe());
   probe_add(line, " ");
   probe_add(line, what);
}

/* Ends a line: keeps a copy for the on-screen readout and appends it to
   ./save/probe.log, fsync'd each time so nothing is lost if Retroplayer
   kills the process. Capped at 6000 lines so the 4 MiB save area can't
   fill up. */
static void probe_finish(struct probe_line *line)
{
   if (probe_line_count >= 6000)
      return;
   ++probe_line_count;
   /* Keep the last few lines for the on-screen readout (capitals only, as
      the small bitmap font has no lower case). */
   char *recent = probe_recent[probe_recent_next];
   probe_recent_next = (probe_recent_next + 1) % 5;
   unsigned index = 0;
   frame_dirty = true;
   for (; index < line->length && index < sizeof(probe_recent[0]) - 1; ++index)
   {
      char character = line->text[index];
      recent[index] = character >= 'a' && character <= 'z' ?
         (char)(character - 32) : character;
   }
   recent[index] = 0;
   line->text[line->length++] = '\n';
   if (probe_fd < 0)
      probe_fd = system_call(SYS_openat, AT_FDCWD, (long)"./save/probe.log",
                             O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644, 0, 0);
   if (probe_fd >= 0)
   {
      system_call(SYS_write, probe_fd, (long)line->text, line->length, 0, 0, 0);
      system_call(SYS_fsync, probe_fd, 0, 0, 0, 0, 0);
   }
}

/* Logs a one-word event, e.g. which retro_* function was called. */
static void probe_event(const char *what)
{
   struct probe_line line;
   probe_begin(&line, what);
   probe_finish(&line);
}

/* Logs an event with a number after it. */
static void probe_event_number(const char *what, unsigned long value)
{
   struct probe_line line;
   probe_begin(&line, what);
   probe_add(&line, " ");
   probe_number(&line, value);
   probe_finish(&line);
}
static void draw_probe(void);
/* PROBE("x") logs in the probe build and compiles to nothing otherwise. */
#define PROBE(what) probe_event(what)
#else
#define PROBE(what) ((void)0)
#endif

/* ---- 4. Drawing primitives -------------------------------------------- */

/* Colours are RGB565: 5 bits red, 6 green, 5 blue in one 16-bit word
   (0xf800 red, 0x07e0 green, 0x001f blue, 0xffff white). Everything on
   screen is built from filled rectangles. */

/* Fills a rectangle in the back buffer, clipped to the screen. */
static void fill(int x, int y, int width, int height, uint16_t color)
{
   int left = x < 0 ? 0 : x;
   int top = y < 0 ? 0 : y;
   int right = x + width > SCREEN_WIDTH ? SCREEN_WIDTH : x + width;
   int bottom = y + height > SCREEN_HEIGHT ? SCREEN_HEIGHT : y + height;
   for (int row = top; row < bottom; ++row)
      for (int column = left; column < right; ++column)
         drawing_frame[row * SCREEN_WIDTH + column] = color;
}

/* Draws a rectangular frame of the given thickness (four thin fills). */
static void outline(int x, int y, int width, int height, int thickness, uint16_t color)
{
   fill(x, y, width, thickness, color);
   fill(x, y + height - thickness, width, thickness, color);
   fill(x, y, thickness, height, color);
   fill(x + width - thickness, y, thickness, height, color);
}

/* Draws text in the small 5x7 bitmap font (snake_font.c): each set bit of a
   glyph row becomes a scale x scale block, and letters are 6 cells apart
   (5 wide plus a 1-cell gap). Used for the probe readout, the SLOW sign and
   as a fallback when the smooth font can't be loaded. */
static void text(const char *message, int x, int y, int scale, uint16_t color)
{
   for (unsigned letter = 0; message[letter]; ++letter)
   {
      const uint8_t *glyph = snake_glyph(message[letter]);
      for (int row = 0; row < 7; ++row)
         for (int column = 0; column < 5; ++column)
            if (glyph[row] & (1u << (4 - column)))
               fill(x + (int)letter * 6 * scale + column * scale,
                    y + row * scale, scale, scale, color);
   }
}

/* Draws anti-aliased text from the smooth font atlas (an 8-bit coverage
   map per glyph, loaded from emu/ by snake_smooth_font.c), scaled to
   cell_height pixels tall with nearest-neighbour sampling. cell_width is
   the width of a full 96-pixel-wide atlas cell; each glyph is narrowed to
   its own inked width. Partly covered pixels are blended with what is
   already in the buffer, channel by channel in RGB565 ("+ 127" rounds the
   division by 255). Falls back to the bitmap font if the atlas is missing. */
static void smooth_text(const char *message, int x, int y, int cell_width,
                        int cell_height, uint16_t color)
{
   if (!snake_smooth_font_load())
   {
      text(message, x, y, cell_height / 9, color);
      return;
   }
   int cursor = x;
   for (int letter = 0; message[letter]; ++letter)
   {
      const uint8_t *glyph = snake_smooth_glyph(message[letter]);
      if (!glyph)
         continue;
      int source_left;
      int source_width;
      snake_smooth_glyph_bounds(message[letter], &source_left, &source_width);
      int width = (source_width * cell_width + 48) / 96;
      for (int row = 0; row < cell_height; ++row)
      {
         int target_y = y + row;
         if (target_y < 0 || target_y >= SCREEN_HEIGHT)
            continue;
         int source_y = row * SNAKE_FONT_HEIGHT / cell_height;
         for (int column = 0; column < width; ++column)
         {
            int target_x = cursor + column;
            if (target_x < 0 || target_x >= SCREEN_WIDTH)
               continue;
            int source_x = source_left + column * source_width / width;
            unsigned alpha = glyph[source_y * SNAKE_FONT_WIDTH + source_x];
            if (!alpha)
               continue;
            uint16_t *pixel = drawing_frame + target_y * SCREEN_WIDTH + target_x;
            if (alpha == 255)
            {
               *pixel = color;
               continue;
            }
            unsigned inverse = 255 - alpha;
            uint16_t previous = *pixel;
            unsigned red = (((previous >> 11) & 31) * inverse +
                            ((color >> 11) & 31) * alpha + 127) / 255;
            unsigned green = (((previous >> 5) & 63) * inverse +
                              ((color >> 5) & 63) * alpha + 127) / 255;
            unsigned blue = ((previous & 31) * inverse +
                             (color & 31) * alpha + 127) / 255;
            *pixel = (uint16_t)((red << 11) | (green << 5) | blue);
         }
      }
      cursor += snake_smooth_advance(message[letter], cell_width);
   }
}

/* Smooth text centred horizontally on the screen. */
static void banner_text(const char *message, int y, int cell_width,
                        int cell_height, uint16_t color)
{
   /* Load the font first so the very first frame measures real glyphs. */
   snake_smooth_font_load();
   int width = 0;
   for (int letter = 0; message[letter]; ++letter)
      width += snake_smooth_advance(message[letter], cell_width);
   smooth_text(message, (SCREEN_WIDTH - width) / 2, y,
               cell_width, cell_height, color);
}

/* ---- 5. Small game helpers -------------------------------------------- */

/* True if a character may appear in saved initials (used to validate files). */
static bool valid_name_character(char character)
{
   for (unsigned index = 0; index < NAME_CHARACTER_COUNT; ++index)
      if (name_characters[index] == character)
         return true;
   return false;
}

/* Plays a sound panned to where the snake's head is. */
static void play_sound(enum snake_sound sound)
{
   snake_audio_play(sound, game.body[0].x, 0);
}

/* Frames per snake move at the current speed level (at 60 fps). */
static unsigned movement_period(void)
{
   /* 8 frames per move at level 0 down to 4 at the top level. The game
      raises the level every five foods; a slow sign lowers it. */
   return 8 - (game.speed_level > 4 ? 4 : game.speed_level);
}

/* Reads CLOCK_MONOTONIC (never jumps with wall-clock changes) in
   nanoseconds; 0 means the clock could not be read, and callers treat 0 as
   "no clock" and fall back to counting frames. */
static uint64_t monotonic_nanoseconds(void)
{
   struct { long seconds; long nanoseconds; } timestamp = {0};
   if (system_call(SYS_clock_gettime, CLOCK_MONOTONIC, (long)&timestamp, 0, 0, 0, 0) != 0)
      return 0;
   return (uint64_t)timestamp.seconds * 1000000000u +
      (uint64_t)timestamp.nanoseconds;
}

/* The time between moves: movement_period() frames of 1/60 s. */
static uint64_t movement_interval_ns(void)
{
   return (uint64_t)movement_period() * 1000000000u / 60u;
}

/* Position of a character in name_characters (0, 'A', if not found). */
static unsigned name_character_index(char character)
{
   for (unsigned index = 0; index < NAME_CHARACTER_COUNT; ++index)
      if (name_characters[index] == character)
         return index;
   return 0;
}

/* One step of initials entry: up/down cycles the current letter through
   name_characters (wrapping), left/right moves between the three letters.
   Clicks only if something actually changed. */
static void name_input(enum snake_direction direction)
{
   if (direction == SNAKE_UP || direction == SNAKE_DOWN)
   {
      unsigned index = name_character_index(entry_name[entry_cursor]);
      index = (index + (direction == SNAKE_UP ? 1 : NAME_CHARACTER_COUNT - 1)) %
         NAME_CHARACTER_COUNT;
      entry_name[entry_cursor] = name_characters[index];
   }
   else if (direction == SNAKE_LEFT && entry_cursor > 0)
      --entry_cursor;
   else if (direction == SNAKE_RIGHT && entry_cursor + 1 < NAME_LENGTH)
      ++entry_cursor;
   else
      return;
   snake_audio_play(SNAKE_SOUND_LETTER, SNAKE_COLUMNS / 2, 0);
   frame_dirty = true;
}

/* Stick, trackball and flippers all arrive here as directions. */
static void steer(enum snake_direction direction)
{
   if (attract)
      return;
   if (entering_name)
      name_input(direction);
   else
      snake_steer(&game, direction);
}

/* ---- 6. Confetti ------------------------------------------------------ */

/* xorshift32: a tiny pseudo-random generator, good enough for confetti. */
static uint32_t confetti_next(void)
{
   confetti_random ^= confetti_random << 13;
   confetti_random ^= confetti_random >> 17;
   confetti_random ^= confetti_random << 5;
   return confetti_random;
}

/* Starts one confetti piece at pixel (x, y) with velocity (vx, vy) in
   1/256 pixel per frame, in a free slot (silently dropped if all 160 are in
   the air), with a random colour, size and tumble phase. */
static void launch_confetti(int32_t x, int32_t y, int32_t vx, int32_t vy)
{
   static const uint16_t colors[] = {
      0xf800, 0xffe0, 0x07ff, 0xf81f, 0x07e0, 0xfc00, 0xffff, 0x7e43
   };
   for (int index = 0; index < CONFETTI_COUNT; ++index)
   {
      struct confetti *piece = &confetti[index];
      if (piece->live)
         continue;
      piece->live = true;
      piece->x = x * 256;
      piece->y = y * 256;
      piece->vx = vx;
      piece->vy = vy;
      piece->color = colors[confetti_next() % 8];
      piece->width = (uint8_t)(10 + confetti_next() % 8);
      piece->height = (uint8_t)(6 + confetti_next() % 5);
      piece->flip = (uint8_t)(confetti_next() % 24);
      confetti_active = true;
      return;
   }
}

/* A random number from low to high inclusive. */
static int32_t spread(int32_t low, int32_t high)
{
   return low + (int32_t)(confetti_next() % (uint32_t)(high - low + 1));
}

/* Two cannons in the bottom corners of the board, aimed up and inward. */
static void fire_confetti_cannons(void)
{
   int bottom = BOARD_Y + SNAKE_ROWS * CELL_SIZE - 20;
   /* 60 pieces per side. Upward speeds of 24-32 px/frame (6200-8200 / 256)
      against 70/256 px/frame^2 gravity carry them up most of the board. */
   for (int index = 0; index < 60; ++index)
   {
      launch_confetti(BOARD_X + 20, bottom, spread(400, 2600), spread(-8200, -6200));
      launch_confetti(BOARD_X + SNAKE_COLUMNS * CELL_SIZE - 20, bottom,
                      -spread(400, 2600), spread(-8200, -6200));
   }
}

/* Moves the confetti one frame: gravity, a soft terminal velocity, air drag
   on sideways speed and a flutter, dropping pieces that leave the screen.
   Redraws every second frame while any are flying (30 fps is plenty and
   halves the drawing cost), and once more when the last one is gone. */
static void update_confetti(void)
{
   if (!confetti_active)
      return;
   /* A gentle rain keeps falling while initials are being entered. */
   if (entering_name && frame_count % 3 == 0)
      launch_confetti(spread(BOARD_X, BOARD_X + SNAKE_COLUMNS * CELL_SIZE),
                      BOARD_Y - 30, spread(-200, 200), spread(200, 700));
   bool any = false;
   for (int index = 0; index < CONFETTI_COUNT; ++index)
   {
      struct confetti *piece = &confetti[index];
      if (!piece->live)
         continue;
      piece->vy += 70;   /* gravity */
      /* Falling faster than ~5 px/frame: halve the excess (air resistance). */
      if (piece->vy > 1300)
         piece->vy = 1300 + (piece->vy - 1300) / 2;
      piece->vx -= piece->vx / 40;   /* sideways drag, 2.5% per frame */
      /* Sideways flutter once the piece is drifting down. */
      int32_t flutter = piece->vy > 0 ?
         ((piece->flip + frame_count / 2) % 24 < 12 ? 160 : -160) : 0;
      piece->x += piece->vx + flutter;
      piece->y += piece->vy;
      if (piece->y / 256 > SCREEN_HEIGHT || piece->x < -20 * 256 ||
          piece->x / 256 > SCREEN_WIDTH + 20)
         piece->live = false;
      else
         any = true;
   }
   confetti_active = any || entering_name;
   if (confetti_active && frame_count % 2 == 0)
      frame_dirty = true;
   else if (!confetti_active)
      frame_dirty = true;
}

/* Draws each live piece as a small rectangle whose width cycles through
   full, half and edge-on (3 px) to look like it is spinning. */
static void draw_confetti(void)
{
   for (int index = 0; index < CONFETTI_COUNT; ++index)
   {
      const struct confetti *piece = &confetti[index];
      if (!piece->live)
         continue;
      /* Pieces narrow and widen as they tumble. */
      unsigned turn = (piece->flip + frame_count / 3) % 8;
      int width = turn < 2 || turn > 5 ? piece->width :
         turn == 2 || turn == 5 ? piece->width / 2 : 3;
      fill(piece->x / 256 - width / 2, piece->y / 256, width, piece->height,
           piece->color);
   }
}

/* ---- 7. High-score table --------------------------------------------- */

/* Where a score would go in the table, or -1 if it doesn't make the top ten.
   Ties go below existing entries. */
static int high_score_rank(unsigned score)
{
   if (!score)
      return -1;
   for (int rank = 0; rank < HIGH_SCORE_COUNT; ++rank)
      if (score > high_scores[rank].score)
         return rank;
   return -1;
}

/* The best score and its initials, as the header, backglass and attract
   screen show them, always come from the top of the table. */
static void sync_best_from_table(void)
{
   if (high_scores[0].score > game.best || !attract)
      game.best = high_scores[0].score;
   for (unsigned index = 0; index < NAME_LENGTH; ++index)
      best_name[index] = high_scores[0].name[index];
   best_name[NAME_LENGTH] = 0;
}

/* Begins initials entry for a score that earned place `rank` (0 = first),
   pre-filled with the initials used last time. A new number one also gets
   the confetti cannons. */
static void start_name_entry(int rank)
{
   entering_name = true;
   entry_rank = rank;
   last_game_rank = rank;
   last_game_record = rank == 0;
   entry_cursor = 0;
   held_direction = -1;
   for (unsigned index = 0; index < NAME_LENGTH; ++index)
      entry_name[index] = last_initials[0] ? last_initials[index] : 'A';
   entry_name[NAME_LENGTH] = 0;
   if (rank == 0)
      fire_confetti_cannons();
}

/* Commits the entered initials: shifts lower places down one, writes the
   new entry, remembers the initials for next time and saves the table to
   disk straight away. */
static void finish_name_entry(void)
{
   entering_name = false;
   if (entry_rank >= 0 && entry_rank < HIGH_SCORE_COUNT)
   {
      for (int rank = HIGH_SCORE_COUNT - 1; rank > entry_rank; --rank)
         high_scores[rank] = high_scores[rank - 1];
      high_scores[entry_rank].score = last_score;
      for (unsigned index = 0; index < 4; ++index)
         high_scores[entry_rank].name[index] =
            index < NAME_LENGTH ? entry_name[index] : 0;
      newest_rank = entry_rank;
   }
   for (unsigned index = 0; index <= NAME_LENGTH; ++index)
      last_initials[index] = entry_name[index];
   entry_rank = -1;
   sync_best_from_table();
   save_high_score();
   high_score_dirty = false;
   frame_dirty = true;
}

/* Fills the table with its factory defaults (a few sample scores). */
static void reset_high_scores(void)
{
   /* A few sample scores so the table looks lived-in from the start. */
   static const struct high_score_entry samples[4] = {
      { 600, "PFI" }, { 500, "SNK" }, { 300, "HOT" }, { 150, "KLR" }
   };
   for (int rank = 0; rank < HIGH_SCORE_COUNT; ++rank)
   {
      high_scores[rank].score = rank < 4 ? samples[rank].score : 0;
      for (unsigned index = 0; index < 4; ++index)
         high_scores[rank].name[index] = rank < 4 ? samples[rank].name[index] : 0;
   }
}

/* A simple rotate-and-xor checksum over every score and name byte, to spot
   a corrupted or hand-edited file. Not cryptographic, just a sanity check. */
static uint32_t high_score_check(const struct high_score_entry *entries)
{
   uint32_t check = 0x5a17c3e5u;
   for (int rank = 0; rank < HIGH_SCORE_COUNT; ++rank)
   {
      check = (check << 5 | check >> 27) ^ entries[rank].score;
      for (unsigned index = 0; index < 4; ++index)
         check = (check << 3 | check >> 29) ^ (uint8_t)entries[rank].name[index];
   }
   return check;
}

/* An entry is plausible if its score is reachable (scores always end in 0,
   max SNAKE_MAX_SCORE) and, unless empty, its initials use allowed letters. */
static bool valid_entry(const struct high_score_entry *entry)
{
   if (entry->score > SNAKE_MAX_SCORE || entry->score % 10)
      return false;
   if (!entry->score)
      return true;
   for (unsigned index = 0; index < NAME_LENGTH; ++index)
      if (!valid_name_character(entry->name[index]))
         return false;
   return true;
}

/* Puts an older single best score into the table ('???' if it had no
   initials). */
static void insert_old_best(uint32_t score, const char *name)
{
   int rank = high_score_rank(score);
   if (rank < 0)
      return;
   for (int place = HIGH_SCORE_COUNT - 1; place > rank; --place)
      high_scores[place] = high_scores[place - 1];
   high_scores[rank].score = score;
   for (unsigned index = 0; index < 4; ++index)
      high_scores[rank].name[index] = index < NAME_LENGTH ?
         (name && name[0] ? name[index] : '?') : 0;
}

/* The current hiscore.dat layout ("SNK3"): all ten entries plus checksum. */
struct high_score_table_record {
   uint32_t magic;
   uint32_t count;
   struct high_score_entry entries[HIGH_SCORE_COUNT];
   uint32_t check;
};

/* Loads save/hiscore.dat. The file is accepted only if magic, count,
   checksum, every entry and the descending order all check out; otherwise
   the defaults stay. Files from older versions (a single best score) are
   recognised by size and magic and merged into the default table. The
   union lets one read() fill whichever layout the file turns out to be. */
static void load_high_score(void)
{
   reset_high_scores();
   union {
      struct high_score_table_record table;
      struct high_score_record single;
   } record;
   long descriptor = system_call(SYS_openat, AT_FDCWD, (long)HIGH_SCORE_PATH,
                                 O_RDONLY | O_CLOEXEC, 0, 0, 0);
   if (descriptor >= 0)
   {
      long bytes = system_call(SYS_read, descriptor, (long)&record, sizeof(record), 0, 0, 0);
      system_call(SYS_close, descriptor, 0, 0, 0, 0, 0);
      if (bytes == sizeof(record.table) &&
          record.table.magic == HIGH_SCORE_TABLE_MAGIC &&
          record.table.count == HIGH_SCORE_COUNT &&
          record.table.check == high_score_check(record.table.entries))
      {
         bool valid = true;
         for (int rank = 0; rank < HIGH_SCORE_COUNT; ++rank)
            valid = valid && valid_entry(&record.table.entries[rank]) &&
               (rank == 0 || record.table.entries[rank].score <=
                record.table.entries[rank - 1].score);
         if (valid)
            for (int rank = 0; rank < HIGH_SCORE_COUNT; ++rank)
               high_scores[rank] = record.table.entries[rank];
      }
      else
      {
         /* Earlier versions kept just the best score (and later its
            initials); carry it into the table. */
         bool old_format = bytes == 12 && record.single.magic == HIGH_SCORE_MAGIC;
         bool named = bytes == sizeof(record.single) &&
            record.single.magic == HIGH_SCORE_NAME_MAGIC;
         if ((old_format || named) &&
             record.single.best_inverse == ~record.single.best &&
             record.single.best <= SNAKE_MAX_SCORE && record.single.best % 10 == 0)
         {
            char name[4] = { 0, 0, 0, 0 };
            if (named)
            {
               bool ok = true;
               for (unsigned index = 0; index < NAME_LENGTH; ++index)
                  ok = ok && valid_name_character(record.single.name[index]);
               for (unsigned index = 0; ok && index < NAME_LENGTH; ++index)
                  name[index] = record.single.name[index];
            }
            insert_old_best(record.single.best, name);
         }
      }
   }
   game.best = high_scores[0].score;
   sync_best_from_table();
}

/* Saves the table atomically: write save/hiscore.tmp, fsync it to the
   flash, then rename() it over hiscore.dat. rename is atomic, so after a
   power cut the file is either the old table or the new one, never half of
   each. If anything fails the temp file is removed and the old file kept. */
static void save_high_score(void)
{
   struct high_score_table_record record;
   record.magic = HIGH_SCORE_TABLE_MAGIC;
   record.count = HIGH_SCORE_COUNT;
   for (int rank = 0; rank < HIGH_SCORE_COUNT; ++rank)
      record.entries[rank] = high_scores[rank];
   record.check = high_score_check(record.entries);
   long descriptor = system_call(SYS_openat, AT_FDCWD, (long)HIGH_SCORE_TEMP_PATH,
                                 O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644, 0, 0);
   if (descriptor < 0)
      return;
   long bytes = system_call(SYS_write, descriptor, (long)&record, sizeof(record), 0, 0, 0);
   long synced = bytes == sizeof(record) ?
      system_call(SYS_fsync, descriptor, 0, 0, 0, 0, 0) : -1;
   system_call(SYS_close, descriptor, 0, 0, 0, 0, 0);
   if (synced == 0 && system_call(SYS_renameat, AT_FDCWD, (long)HIGH_SCORE_TEMP_PATH,
                                  AT_FDCWD, (long)HIGH_SCORE_PATH, 0, 0) == 0)
      return;
   system_call(SYS_unlinkat, AT_FDCWD, (long)HIGH_SCORE_TEMP_PATH, 0, 0, 0, 0);
}

/* ---- 8. Backglass ----------------------------------------------------- */

/* Publishes what the backglass screen needs (score, best, phase, initials,
   whether a trackball was found...) to /tmp/alp-snake-state.bin for the
   alp_snake_backglass helper. Written to a temp file and renamed into
   place so the helper never reads a half-written struct. sequence is the
   frame count, so the file always changes while the game is running. */
static void write_backglass_state(void)
{
   struct backglass_state state = {
      SNAKE_STATE_MAGIC, attract ? last_score : game.score, game.best,
      (uint32_t)game.phase,
      (uint32_t)game.direction, game.length, game.steps,
      trackball_descriptor >= 0, frame_count, {0, 0, 0, 0},
      entering_name ? (entry_rank == 0 ? SNAKE_STATE_ENTERING_NAME :
                       SNAKE_STATE_TOP_TEN) : 0u
   };
   for (unsigned index = 0; index < NAME_LENGTH; ++index)
      state.name[index] = entering_name ? entry_name[index] : best_name[index];
   long descriptor = system_call(SYS_openat, AT_FDCWD, (long)SNAKE_STATE_TEMP_PATH,
                                 O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644, 0, 0);
   if (descriptor < 0)
      return;
   long written = system_call(SYS_write, descriptor, (long)&state, sizeof(state), 0, 0, 0);
   system_call(SYS_close, descriptor, 0, 0, 0, 0, 0);
   if (written == sizeof(state))
      system_call(SYS_renameat, AT_FDCWD, (long)SNAKE_STATE_TEMP_PATH, AT_FDCWD, (long)SNAKE_STATE_PATH, 0, 0);
}

/* Runs in the freshly cloned backglass child: closes every file descriptor
   above stderr that it inherited from Retroplayer (open devices, sockets,
   the trackball...), so the helper doesn't hold them open. Lists
   /proc/self/fd with getdents64 and parses the raw linux_dirent64 records:
   d_reclen is the 16-bit little-endian field at byte 16, the name starts at
   byte 19. If /proc isn't available it just closes 3..1023 blindly. */
static void close_inherited_descriptors(void)
{
   char entries[1024];
   long directory = system_call(SYS_openat, AT_FDCWD, (long)"/proc/self/fd",
                                O_RDONLY | O_DIRECTORY | O_CLOEXEC, 0, 0, 0);
   if (directory < 0)
   {
      for (long descriptor = 3; descriptor < 1024; ++descriptor)
         system_call(SYS_close, descriptor, 0, 0, 0, 0, 0);
      return;
   }
   for (;;)
   {
      long bytes = system_call(SYS_getdents64, directory, (long)entries, sizeof(entries), 0, 0, 0);
      if (bytes <= 0)
         break;
      for (long offset = 0; offset < bytes;)
      {
         unsigned short length = (unsigned char)entries[offset + 16] |
                                 (unsigned short)(unsigned char)entries[offset + 17] << 8;
         if (length < 20 || offset + length > bytes)
            break;
         const char *name = entries + offset + 19;
         long descriptor = 0;
         for (unsigned index = 0; name[index] >= '0' && name[index] <= '9'; ++index)
            descriptor = descriptor * 10 + name[index] - '0';
         if (name[0] >= '0' && name[0] <= '9' &&
             descriptor >= 3 && descriptor != directory)
            system_call(SYS_close, descriptor, 0, 0, 0, 0, 0);
         offset += length;
      }
   }
   system_call(SYS_close, directory, 0, 0, 0, 0, 0);
}

/* Starts the backglass helper, ./emu/alp_snake_backglass, which draws the
   cabinet's second (backglass) screen with SDL2. clone(SIGCHLD) with no
   other flags behaves like fork(). In the child:
     - PR_SET_PDEATHSIG makes the kernel send SIGTERM if the core's process
       dies, so the helper never outlives the game; the getppid check
       covers the parent dying before prctl took effect;
     - inherited descriptors are closed;
     - execve starts the helper with a minimal environment, where
       ForceConnectID=93 makes the cabinet's SDL pick the backglass display.
   If execve fails the child exits with 127 (the shell's "not found").
   Only raw system calls are used in the child, which is safe after a fork
   of a multi-threaded process. */
static void launch_backglass(void)
{
   static char program[] = "./emu/alp_snake_backglass";
   static char *const arguments[] = {program, 0};
   static char environment_display[] = "ForceConnectID=93";
   static char environment_home[] = "HOME=/";
   static char environment_path[] = "PATH=/bin:/usr/bin:/sbin:/usr/sbin";
   static char *const environment[] = {
      environment_display, environment_home, environment_path, 0
   };
   long parent = system_call(SYS_getpid, 0, 0, 0, 0, 0, 0);
   long process = system_call(SYS_clone, SIGCHLD, 0, 0, 0, 0, 0);
   if (process == 0)
   {
      if (system_call(SYS_prctl, PR_SET_PDEATHSIG, SIGTERM, 0, 0, 0, 0) < 0 ||
          system_call(SYS_getppid, 0, 0, 0, 0, 0, 0) != parent)
         system_call(SYS_exit, 127, 0, 0, 0, 0, 0);
      close_inherited_descriptors();
      system_call(SYS_execve, (long)program, (long)arguments, (long)environment, 0, 0, 0);
      system_call(SYS_exit, 127, 0, 0, 0, 0, 0);
   }
   backglass_process = process;
}

/* Stops the backglass helper. Deleting the state file is the helper's
   normal signal to quit; SIGTERM backs that up, and the loop waits up to
   ~200 ms (40 x 5 ms) to reap the child so no zombie is left behind. */
static void stop_backglass(void)
{
   system_call(SYS_unlinkat, AT_FDCWD, (long)SNAKE_STATE_PATH, 0, 0, 0, 0);
   system_call(SYS_unlinkat, AT_FDCWD, (long)SNAKE_STATE_TEMP_PATH, 0, 0, 0, 0);
   if (backglass_process > 0)
   {
      system_call(SYS_kill, backglass_process, SIGTERM, 0, 0, 0, 0);
      for (int attempt = 0; attempt < 40; ++attempt)
      {
         long result = system_call(SYS_wait4, backglass_process, 0, WNOHANG, 0, 0, 0);
         if (result != 0)
            break;
         long interval[2] = {0, 5000000};
         system_call(SYS_nanosleep, (long)interval, 0, 0, 0, 0, 0);
      }
   }
   backglass_process = -1;
}

/* ---- 9. Trackball ----------------------------------------------------- */

/* Retroplayer doesn't pass the cabinet's trackball to the core, so we look
   for it ourselves: open each /dev/input/event0..31 (non-blocking) and ask
   with EVIOCGBIT(EV_REL) which relative axes it reports. The first device
   with both REL_X and REL_Y (bits 0 and 1) is taken as the trackball.
   Rescans only every 120 frames (2 s) while none is open, since opening 32
   devices every frame would be wasteful; it also handles hot-plugging. */
static void find_trackball(void)
{
   if (trackball_descriptor >= 0 || frame_count % 120)
      return;
   for (int index = 0; index < 32; ++index)
   {
      char path[] = "/dev/input/event00";   /* digits at [16], [17] */
      if (index < 10)
      {
         path[16] = (char)('0' + index);
         path[17] = 0;
      }
      else
      {
         path[16] = (char)('0' + index / 10);
         path[17] = (char)('0' + index % 10);
      }
      long descriptor = system_call(SYS_openat, AT_FDCWD, (long)path, O_RDONLY | O_NONBLOCK | O_CLOEXEC, 0, 0, 0);
      if (descriptor < 0)
         continue;
      uint64_t relative_bits = 0;
      long result = system_call(SYS_ioctl, descriptor, ALP_EVIOCGBIT_REL, (long)&relative_bits, 0, 0, 0);
      if (result >= 0 && (relative_bits & 3) == 3)
      {
         trackball_descriptor = descriptor;
         frame_dirty = true;
         return;
      }
      system_call(SYS_close, descriptor, 0, 0, 0, 0, 0);
   }
}

/* Drains all pending trackball events and turns accumulated motion into a
   steering direction. Events with type 2 (EV_REL) and code 0/1 (REL_X/
   REL_Y) are summed. Once either axis reaches 35 counts the dominant axis
   wins and the sum is cleared; otherwise the sums decay by a quarter each
   frame, so slow drift or a light touch never adds up to a turn. A read
   result of -19 (ENODEV, unplugged) or 0 closes the device so
   find_trackball() can look again; -4 (EINTR) retries; -11 (EAGAIN, no more
   events) ends the loop. At most 64 batches of 32 events per frame. */
static void poll_trackball(void)
{
   if (trackball_descriptor < 0)
      return;
   struct input_event events[32];
   for (int batch = 0; batch < 64; ++batch)
   {
      long bytes = system_call(SYS_read, trackball_descriptor, (long)events, sizeof(events), 0, 0, 0);
      if (bytes == -19 || bytes == 0)
      {
         system_call(SYS_close, trackball_descriptor, 0, 0, 0, 0, 0);
         trackball_descriptor = -1;
         trackball_x = 0;
         trackball_y = 0;
         frame_dirty = true;
         return;
      }
      if (bytes == -4)
         continue;
      if (bytes <= 0)
         break;
      for (long index = 0; index < bytes / (long)sizeof(events[0]); ++index)
         if (events[index].type == 2)
         {
            if (events[index].code == 0)
               trackball_x += events[index].value;
            else if (events[index].code == 1)
               trackball_y += events[index].value;
         }
   }
   if (trackball_x >= 35 || trackball_x <= -35 ||
       trackball_y >= 35 || trackball_y <= -35)
   {
      int absolute_x = trackball_x < 0 ? -trackball_x : trackball_x;
      int absolute_y = trackball_y < 0 ? -trackball_y : trackball_y;
      if (absolute_x > absolute_y)
         steer(trackball_x > 0 ? SNAKE_RIGHT : SNAKE_LEFT);
      else
         steer(trackball_y > 0 ? SNAKE_DOWN : SNAKE_UP);
      trackball_x = 0;
      trackball_y = 0;
   }
   trackball_x = trackball_x * 3 / 4;
   trackball_y = trackball_y * 3 / 4;
}

/* ---- 10. Attract mode and the DIP switch page ------------------------- */

/* Whether a button on the first joypad is held, via Retroplayer's input
   state callback. Cabinet mapping: L2 left flipper, R right flipper,
   Select = Insert Coin, Start, and the joystick as the d-pad. */
static bool pressed(unsigned button)
{
   return input_state_callback &&
      input_state_callback(0, RETRO_DEVICE_JOYPAD, 0, button);
}

/* Clears leftover popups and hawk animations before a new game. */
static void clear_effects(void)
{
   catch_popup_frames = 0;
   hawk_flyoff_frames = 0;
   hawk_has_snake = false;
   hawk_carry_frames = 0;
   snake_lift = 0;
}

/* A fresh demo game: mice only, so it never ends at the hawk's talons. */
static void start_demo(void)
{
   clear_effects();
   snake_start(&game);
   game.features = SNAKE_FEATURE_MOUSE;
   game.best = attract_saved_best;
   demo_frames = 0;
   demo_dead_frames = 0;
   frame_dirty = true;
}

/* Switches to attract mode: remembers the real best score, starts a demo
   game and (if enabled) the music. */
static void enter_attract(void)
{
   attract = true;
   attract_frames = 0;
   attract_saved_best = game.best;
   start_demo();
   if (attract_music_enabled)
      snake_audio_music(1);
}

/* Start a real game, from attract or from the Game Over screen. */
static void start_real_game(void)
{
   if (attract)
   {
      attract = false;
      game.best = attract_saved_best;
      snake_audio_music(0);
   }
   clear_effects();
   best_at_start = game.best;
   last_game_record = false;
   last_game_rank = -1;
   game_over_frames = 0;
   /* Set before and after snake_start: the start-up uses the features,
      and after the demo (mice only) the real settings must be restored. */
   game.features = configured_features;
   snake_start(&game);
   game.features = configured_features;
   play_sound(SNAKE_SOUND_START);
   frame_dirty = true;
}

/* Runs the demo game: the autopilot steers, nothing scores or sounds. */
static void update_attract(void)
{
   if (settings_open)
      return;
   ++attract_frames;
   if (attract_frames % ATTRACT_PANEL_FRAMES == 0 || attract_frames % 30 == 0)
      frame_dirty = true;   /* panel change and the blinking PRESS START */
   if (game.phase != SNAKE_PLAYING)
   {
      if (++demo_dead_frames >= 60)   /* show the crash for 1 s */
         start_demo();
      return;
   }
   if (++demo_frames % ATTRACT_DEMO_PERIOD)
      return;
   enum snake_direction direction = snake_demo_direction(&game);
   if (direction != game.pending_direction)
      snake_steer(&game, direction);
   snake_step(&game);
   game.best = attract_saved_best;   /* the demo's score must not count */
   frame_dirty = true;
   if (game.phase == SNAKE_PLAYING &&
       (game.steps >= ATTRACT_DEMO_MOVES || game.length >= ATTRACT_DEMO_MAX_LENGTH))
      start_demo();
}

static bool coin_was_pressed;
static bool menu_button_was_pressed;    /* R3, tapped by Adv. Config */
static unsigned last_display_setup_change;   /* frame_count of last change */

/* Flips the switch under the cursor (and applies it at once), or closes
   the page if the cursor is on EXIT. */
static void toggle_setting(void)
{
   if (settings_cursor < SETTING_COUNT)
   {
      settings_bits ^= 1u << settings_cursor;
      snake_audio_play(SNAKE_SOUND_LETTER, SNAKE_COLUMNS / 2, 0);
      apply_settings();
   }
   else
      close_settings();
}

/* The DIP switch page: stick up/down picks a switch; left/right, either
   flipper, A/B or Start flips it. Any of those on EXIT (or leaving the
   cabinet menu) closes the page. */
static bool action_buttons_were_pressed;
/* Handles one frame of input while the DIP switch page is open (see the
   comment above). `start` is whether Start is held this frame. */
static void settings_input(bool start)
{
   int direction = pressed(RETRO_DEVICE_ID_JOYPAD_UP) ? SNAKE_UP :
      pressed(RETRO_DEVICE_ID_JOYPAD_DOWN) ? SNAKE_DOWN :
      pressed(RETRO_DEVICE_ID_JOYPAD_LEFT) ? SNAKE_LEFT :
      pressed(RETRO_DEVICE_ID_JOYPAD_RIGHT) ? SNAKE_RIGHT : -1;
   bool step = false;
   if (direction != held_direction)
   {
      held_direction = direction;
      held_frames = 0;
      step = direction >= 0;
   }
   /* Held: repeat after 20 frames (1/3 s), then every 8 frames. */
   else if (direction >= 0 && ++held_frames >= 20 && (held_frames - 20) % 8 == 0)
      step = true;
   if (step)
   {
      if (direction == SNAKE_UP && settings_cursor > 0)
         --settings_cursor;
      else if (direction == SNAKE_DOWN && settings_cursor < SETTING_COUNT)
         ++settings_cursor;
      else if (direction == SNAKE_LEFT || direction == SNAKE_RIGHT)
         toggle_setting();   /* on EXIT this closes the page */
      else
         step = false;
      if (step && (direction == SNAKE_UP || direction == SNAKE_DOWN))
         snake_audio_play(SNAKE_SOUND_LETTER, SNAKE_COLUMNS / 2, 0);
      frame_dirty = true;
   }
   if (!settings_open)
      return;
   bool left = pressed(RETRO_DEVICE_ID_JOYPAD_L2);
   bool right = pressed(RETRO_DEVICE_ID_JOYPAD_R);
   bool action = pressed(RETRO_DEVICE_ID_JOYPAD_A) || pressed(RETRO_DEVICE_ID_JOYPAD_B);
   if ((left && !left_was_pressed) || (right && !right_was_pressed) ||
       (action && !action_buttons_were_pressed) || (start && !start_was_pressed))
      toggle_setting();
   left_was_pressed = left;
   right_was_pressed = right;
   action_buttons_were_pressed = action;
   start_was_pressed = start;
}

/* ---- 11. Input ------------------------------------------------------- */

/* Reads all controls for this frame and acts on them. In priority order:
     1. R3 tap from Adv. Config may reopen the DIP switch page;
     2. while that page is open it gets all input;
     3. Select (Insert Coin) starts a game from any idle screen;
     4. while entering initials: stick spells, flippers move, Start saves;
     5. otherwise: both flippers held on an idle screen show diagnostics,
        Start starts or pauses, the stick steers directly and each flipper
        turns the snake 90 degrees (left flipper anticlockwise, right
        clockwise; directions are numbered UP, RIGHT, DOWN, LEFT so +1 and
        +3 mod 4 are a right and a left turn). The trackball is read last.
   Buttons act on the frame they go down (compared with *_was_pressed). */
static void poll_controls(void)
{
   if (input_poll_callback)
      input_poll_callback();
   /* Retroplayer taps R3 when it opens Adv. Config. If the page was closed
      from inside the game, the "Display MAME menu" setting doesn't change
      on the next Adv. Config, so that tap is what reopens the page. */
   bool menu_button = pressed(RETRO_DEVICE_ID_JOYPAD_R3);
   /* Ignore the tap for 2 s after the variable itself changed: that
      change already opened or closed the page. */
   if (menu_button && !menu_button_was_pressed && !settings_open &&
       display_setup_enabled && frame_count - last_display_setup_change > 120)
      open_settings();
   menu_button_was_pressed = menu_button;
   if (settings_open)
   {
      settings_input(pressed(RETRO_DEVICE_ID_JOYPAD_START));
      return;
   }
   /* Insert Coin in the cabinet menu presses Select: start a game. */
   bool coin = pressed(RETRO_DEVICE_ID_JOYPAD_SELECT);
   if (coin && !coin_was_pressed && !entering_name &&
       (attract || game.phase == SNAKE_READY || game.phase == SNAKE_GAME_OVER ||
        game.phase == SNAKE_WON))
      start_real_game();
   coin_was_pressed = coin;
   bool start = pressed(RETRO_DEVICE_ID_JOYPAD_START);
   if (entering_name)
   {
      if (start && !start_was_pressed)
      {
         finish_name_entry();
         snake_audio_play(SNAKE_SOUND_SAVE, SNAKE_COLUMNS / 2, 0);
      }
      start_was_pressed = start;
      /* Stick: one step per push, then repeats while held. */
      int direction = pressed(RETRO_DEVICE_ID_JOYPAD_UP) ? SNAKE_UP :
         pressed(RETRO_DEVICE_ID_JOYPAD_DOWN) ? SNAKE_DOWN :
         pressed(RETRO_DEVICE_ID_JOYPAD_LEFT) ? SNAKE_LEFT :
         pressed(RETRO_DEVICE_ID_JOYPAD_RIGHT) ? SNAKE_RIGHT : -1;
      if (direction != held_direction)
      {
         held_direction = direction;
         held_frames = 0;
         if (direction >= 0)
            name_input((enum snake_direction)direction);
      }
      /* Repeat after 20 frames, then every 6 (10 letters a second). */
      else if (direction >= 0 && ++held_frames >= 20 && (held_frames - 20) % 6 == 0)
         name_input((enum snake_direction)direction);
      /* Flippers move between letters; the right flipper on the last
         letter saves. */
      bool left = pressed(RETRO_DEVICE_ID_JOYPAD_L2);
      bool right = pressed(RETRO_DEVICE_ID_JOYPAD_R);
      if (entering_name && left && !left_was_pressed)
         name_input(SNAKE_LEFT);
      if (entering_name && right && !right_was_pressed)
      {
         if (entry_cursor + 1 == NAME_LENGTH)
         {
            finish_name_entry();
            snake_audio_play(SNAKE_SOUND_SAVE, SNAKE_COLUMNS / 2, 0);
         }
         else
            name_input(SNAKE_RIGHT);
      }
      left_was_pressed = left;
      right_was_pressed = right;
      poll_trackball();
      return;
   }
   /* Diagnostics only while not actually playing, so holding both flippers
      in a game still just steers. */
   bool both_flippers = pressed(RETRO_DEVICE_ID_JOYPAD_L2) &&
      pressed(RETRO_DEVICE_ID_JOYPAD_R) && (attract || game.phase != SNAKE_PLAYING);
   if (both_flippers != show_diagnostics)
   {
      show_diagnostics = both_flippers;
      frame_dirty = true;
   }
   if (start && !start_was_pressed)
   {
      if (attract || game.phase == SNAKE_READY ||
          game.phase == SNAKE_GAME_OVER || game.phase == SNAKE_WON)
         start_real_game();
      else
      {
         snake_pause(&game);
         play_sound(game.phase == SNAKE_PAUSED ?
                    SNAKE_SOUND_PAUSE : SNAKE_SOUND_RESUME);
      }
      frame_dirty = true;
   }
   start_was_pressed = start;

   if (pressed(RETRO_DEVICE_ID_JOYPAD_UP))
      steer(SNAKE_UP);
   else if (pressed(RETRO_DEVICE_ID_JOYPAD_RIGHT))
      steer(SNAKE_RIGHT);
   else if (pressed(RETRO_DEVICE_ID_JOYPAD_DOWN))
      steer(SNAKE_DOWN);
   else if (pressed(RETRO_DEVICE_ID_JOYPAD_LEFT))
      steer(SNAKE_LEFT);

   bool left = pressed(RETRO_DEVICE_ID_JOYPAD_L2);
   bool right = pressed(RETRO_DEVICE_ID_JOYPAD_R);
   if (left && !left_was_pressed)
      steer((game.pending_direction + 3) % 4);
   if (right && !right_was_pressed)
      steer((game.pending_direction + 1) % 4);
   left_was_pressed = left;
   right_was_pressed = right;
   poll_trackball();
}

/* ---- 12. Sprites and screens ----------------------------------------- */

/* Palette taken from assets/snake-mascot.png (RGB565). */
#define SNAKE_LIME 0x7e43
#define SNAKE_LIME_DARK 0x5d02
#define SNAKE_LIME_LIGHT 0xb727
#define SNAKE_YELLOW 0xfe42
#define SNAKE_IRIS 0xcbc2
#define SNAKE_TONGUE 0xe1ea
#define SNAKE_MOUTH 0x68a2
#define SNAKE_NOSTRIL 0x51e0
#define SNAKE_LID_LINE 0x2ac0
#define BOARD_WIDTH (SNAKE_COLUMNS * CELL_SIZE)
#define BOARD_HEIGHT (SNAKE_ROWS * CELL_SIZE)

/* Fills a rectangle given in head-local coordinates: forward runs from the
   back edge of the head cell (0) toward its front edge (CELL_SIZE) and beyond,
   side runs from the head's left to its right. Output is clipped to the board.
   This lets each sprite (head, tail, mouse) be described once, facing
   "forward", and drawn rotated to any of the four directions for free. */
static void head_rect(int cell_x, int cell_y, enum snake_direction direction,
                      int forward, int side, int length, int width, uint16_t color)
{
   int x;
   int y;
   int w;
   int h;
   if (direction == SNAKE_UP)
   {
      x = cell_x + side;
      y = cell_y + CELL_SIZE - forward - length;
      w = width;
      h = length;
   }
   else if (direction == SNAKE_RIGHT)
   {
      x = cell_x + forward;
      y = cell_y + side;
      w = length;
      h = width;
   }
   else if (direction == SNAKE_DOWN)
   {
      x = cell_x + CELL_SIZE - side - width;
      y = cell_y + forward;
      w = width;
      h = length;
   }
   else
   {
      x = cell_x + CELL_SIZE - forward - length;
      y = cell_y + CELL_SIZE - side - width;
      w = length;
      h = width;
   }
   int left = x < BOARD_X ? BOARD_X : x;
   int top = y < BOARD_Y ? BOARD_Y : y;
   int right = x + w > BOARD_X + BOARD_WIDTH ? BOARD_X + BOARD_WIDTH : x + w;
   int bottom = y + h > BOARD_Y + BOARD_HEIGHT ? BOARD_Y + BOARD_HEIGHT : y + h;
   if (right > left && bottom > top)
      fill(left, top, right - left, bottom - top, color);
}

/* True if the cell just in front of the head holds food, the mouse or a
   SLOW sign: the snake opens its mouth when about to eat. */
static bool food_ahead(void)
{
   int x = game.body[0].x;
   int y = game.body[0].y;
   if (game.direction == SNAKE_UP)
      --y;
   else if (game.direction == SNAKE_RIGHT)
      ++x;
   else if (game.direction == SNAKE_DOWN)
      ++y;
   else
      --x;
   return (x == game.food.x && y == game.food.y) ||
      (game.mouse_active && x == game.mouse.x && y == game.mouse.y) ||
      (game.sign_active && x == game.sign.x && y == game.sign.y);
}

/* Packs the head's current look into one number so retro_run can tell when
   the picture needs redrawing. Bits 0-1: eyes (0 open, 1 blink, 2 crashed),
   bits 2-4: tongue length in 4-pixel steps, bit 5: mouth open. */
static unsigned compute_head_animation(void)
{
   if (game.phase == SNAKE_GAME_OVER)
      return 2;
   if (game.phase != SNAKE_PLAYING)
      return 0;
   unsigned eyes = frame_count % 210 < 7 ? 1 : 0;
   unsigned tongue = 0;
   unsigned mouth = food_ahead() ? 1 : 0;
   unsigned flick = (frame_count + 105) % 150;
   if (mouth)
      tongue = 3;
   else if (flick < 16)
      tongue = (flick < 8 ? flick : 16 - flick) / 2;
   if (tongue > 3)
      tongue = 3;
   return eyes | tongue << 2 | mouth << 5;
}

/* Tapered tail pointing away from the body, ending in the mascot's yellow
   tip. The tip shifts side to side on each move, so it wiggles as the snake
   slithers without needing any extra redraws. */
static void draw_tail(void)
{
   struct snake_point tail = game.body[game.length - 1];
   struct snake_point neighbor = game.body[game.length - 2];
   enum snake_direction d = tail.x > neighbor.x ? SNAKE_RIGHT :
      tail.x < neighbor.x ? SNAKE_LEFT :
      tail.y > neighbor.y ? SNAKE_DOWN : SNAKE_UP;
   int x = BOARD_X + tail.x * CELL_SIZE;
   int y = BOARD_Y + tail.y * CELL_SIZE - snake_lift;
   int wag = game.steps % 2 ? 5 : -5;
   head_rect(x, y, d, 0, 5, 16, 44, SNAKE_LIME);
   head_rect(x, y, d, 16, 10 + wag / 5, 14, 34, SNAKE_LIME);
   head_rect(x, y, d, 30, 15 + wag * 3 / 5, 10, 24, SNAKE_LIME);
   head_rect(x, y, d, 2, 23, 30, 8, SNAKE_LIME_LIGHT);
   head_rect(x, y, d, 40, 20 + wag, 7, 14, SNAKE_YELLOW);
   head_rect(x, y, d, 47, 23 + wag, 5, 8, SNAKE_YELLOW);
}

#define MOUSE_GREY 0xa515
#define MOUSE_LIGHT 0xce59
#define MOUSE_PINK 0xfd57
#define MOUSE_TAIL 0xe474

/* A little grey mouse facing the way it runs; its legs patter and its tail
   swishes on alternate moves. */
static void draw_mouse_at(int x, int y, enum snake_direction d, bool stride)
{
   int swish = stride ? 4 : -4;

   /* Long thin tail trailing into the cell behind. */
   head_rect(x, y, d, -18, 25 + swish, 10, 3, MOUSE_TAIL);
   head_rect(x, y, d, -8, 25 + swish / 2, 10, 3, MOUSE_TAIL);
   head_rect(x, y, d, 2, 25, 10, 3, MOUSE_TAIL);

   /* Feet. */
   head_rect(x, y, d, stride ? 12 : 16, 11, 5, 5, MOUSE_PINK);
   head_rect(x, y, d, stride ? 16 : 12, 38, 5, 5, MOUSE_PINK);
   head_rect(x, y, d, stride ? 30 : 26, 11, 5, 5, MOUSE_PINK);
   head_rect(x, y, d, stride ? 26 : 30, 38, 5, 5, MOUSE_PINK);

   /* Body, belly highlight and head. */
   head_rect(x, y, d, 10, 16, 26, 22, MOUSE_GREY);
   head_rect(x, y, d, 13, 13, 20, 28, MOUSE_GREY);
   head_rect(x, y, d, 15, 22, 14, 10, MOUSE_LIGHT);
   head_rect(x, y, d, 34, 18, 10, 18, MOUSE_GREY);
   head_rect(x, y, d, 44, 21, 4, 12, MOUSE_GREY);
   head_rect(x, y, d, 48, 24, 3, 6, MOUSE_GREY);
   head_rect(x, y, d, 50, 25, 3, 4, MOUSE_PINK);

   /* Round ears with pink insides. */
   for (int ear = 0; ear < 2; ++ear)
   {
      int side = ear ? 33 : 9;
      head_rect(x, y, d, 30, side + 2, 12, 8, MOUSE_GREY);
      head_rect(x, y, d, 32, side, 8, 12, MOUSE_GREY);
      head_rect(x, y, d, 32, side + 3, 8, 6, MOUSE_PINK);
   }

   /* Eyes and whiskers. */
   head_rect(x, y, d, 41, 21, 3, 3, 0x0000);
   head_rect(x, y, d, 41, 30, 3, 3, 0x0000);
   head_rect(x, y, d, 46, 14, 1, 7, MOUSE_LIGHT);
   head_rect(x, y, d, 46, 33, 1, 7, MOUSE_LIGHT);
}

/* Draws the running mouse, if one is on the board. */
static void draw_mouse(void)
{
   if (game.mouse_active)
      draw_mouse_at(BOARD_X + game.mouse.x * CELL_SIZE,
                    BOARD_Y + game.mouse.y * CELL_SIZE,
                    game.mouse_direction, game.mouse_moves & 1);
}

/* A yellow diamond road sign with a black border reading SLOW, drawn into
   the cell at pixel (x, y) as 50 one-pixel rows whose width grows then
   shrinks. 0xff00 is a slightly orange yellow in RGB565. */
static void draw_sign_at(int x, int y)
{
   int center = x + CELL_SIZE / 2;
   for (int row = 0; row < 50; ++row)
   {
      int half = row < 25 ? row : 49 - row;
      fill(center - half, y + 2 + row, half * 2 + 1, 1, 0x0000);
      if (half > 3)
         fill(center - half + 3, y + 2 + row, half * 2 - 5, 1, 0xff00);
   }
   text("SLOW", center - 12, y + 24, 1, 0x0000);
}

/* Draws the SLOW sign on the board. It blinks (hidden on odd counts) for
   its last 14 moves before it disappears. */
static void draw_sign(void)
{
   if (!game.sign_active ||
       (game.sign_steps_left <= 14 && game.sign_steps_left % 2))
      return;
   draw_sign_at(BOARD_X + game.sign.x * CELL_SIZE, BOARD_Y + game.sign.y * CELL_SIZE);
}

/* The short-lived popup where something was eaten or dodged: "SLOWER" for
   a sign, or "+20" (mouse) / "+50" (hawk dodged), kept on the board. */
static void draw_catch_popup(void)
{
   if (!catch_popup_frames)
      return;
   if (popup_is_slow)
   {
      int x = catch_popup_x - 40;
      int y = catch_popup_y;
      if (x < BOARD_X + 6)
         x = BOARD_X + 6;
      if (x > BOARD_X + SNAKE_COLUMNS * CELL_SIZE - 210)
         x = BOARD_X + SNAKE_COLUMNS * CELL_SIZE - 210;
      if (y < BOARD_Y + 10)
         y = BOARD_Y + 10;
      smooth_text("SLOWER", x, y, 40, 56, 0xff00);
      return;
   }
   /* A plus sign and "20", nudged to stay on the board. */
   int x = catch_popup_x;
   int y = catch_popup_y;
   if (x > BOARD_X + SNAKE_COLUMNS * CELL_SIZE - 150)
      x = BOARD_X + SNAKE_COLUMNS * CELL_SIZE - 150;
   if (y < BOARD_Y + 10)
      y = BOARD_Y + 10;
   fill(x, y + 25, 30, 10, 0xffe0);
   fill(x + 10, y + 15, 10, 30, 0xffe0);
   smooth_text(popup_points, x + 36, y, 48, 62, 0xffe0);
}

/* The hawk, seen from above with wings spread, head toward the top of the
   screen. Each entry is a rectangle relative to the hawk's centre at full
   size: x, y, width, height, colour. */
#define HAWK_BROWN 0x7a43
#define HAWK_DARK 0x4142
#define HAWK_LIGHT 0xcd0d
#define HAWK_BEAK 0xfea0
static const int32_t hawk_parts[][5] = {
   {-32, -18, 64, 36, HAWK_BROWN},          /* inner wings */
   {-62, -16, 32, 32, HAWK_BROWN}, {30, -16, 32, 32, HAWK_BROWN},
   {-84, -13, 24, 26, HAWK_BROWN}, {60, -13, 24, 26, HAWK_BROWN},
   {-96, -11, 14, 9, HAWK_DARK}, {82, -11, 14, 9, HAWK_DARK},  /* wingtip feathers */
   {-94, -1, 12, 9, HAWK_DARK}, {82, -1, 12, 9, HAWK_DARK},
   {-90, 9, 10, 8, HAWK_DARK}, {80, 9, 10, 8, HAWK_DARK},
   {-80, 14, 12, 7, HAWK_DARK}, {68, 14, 12, 7, HAWK_DARK},
   {-62, -16, 30, 5, HAWK_LIGHT}, {32, -16, 30, 5, HAWK_LIGHT}, /* leading edges */
   {-12, -26, 24, 52, HAWK_BROWN},          /* body */
   {-6, -16, 12, 30, HAWK_LIGHT},           /* breast */
   {-16, 26, 32, 18, HAWK_BROWN},           /* tail */
   {-16, 32, 32, 3, HAWK_DARK}, {-16, 39, 32, 3, HAWK_DARK},
   {-9, -42, 18, 18, 0xffff},               /* white head */
   {-3, -49, 6, 8, HAWK_BEAK},
   {-6, -37, 3, 3, 0x0000}, {3, -37, 3, 3, 0x0000}
};
#define HAWK_PART_COUNT (sizeof(hawk_parts) / sizeof(hawk_parts[0]))
static const int32_t hawk_talons[][5] = {
   {-14, 24, 6, 14, HAWK_BEAK}, {8, 24, 6, 14, HAWK_BEAK},
   {-17, 36, 3, 5, 0x0000}, {-12, 37, 3, 5, 0x0000},
   {8, 37, 3, 5, 0x0000}, {13, 36, 3, 5, 0x0000}
};

/* Shadow mask, so overlapping parts darken the board only once. */
#define HAWK_MASK_WIDTH 300
#define HAWK_MASK_HEIGHT 160
static uint8_t hawk_mask[HAWK_MASK_HEIGHT][HAWK_MASK_WIDTH];

/* Draws the hawk centred at (center_x, center_y), scaled by scale/100.
   With shadow set it draws only a shadow: the body parts are first
   rasterised into hawk_mask, then every masked board pixel is darkened to
   half brightness. ">> 1 & 0x7bef" halves R, G and B at once in RGB565;
   the mask makes sure overlapping parts don't darken a pixel twice. The
   mask is 300x160, enough for the 192-pixel wingspan at up to 150%. With
   talons set, yellow legs are added (carrying the snake or flying off). */
static void draw_hawk(int center_x, int center_y, int scale, bool shadow, bool talons)
{
   if (shadow)
   {
      for (int row = 0; row < HAWK_MASK_HEIGHT; ++row)
         for (int column = 0; column < HAWK_MASK_WIDTH; ++column)
            hawk_mask[row][column] = 0;
      for (unsigned part = 0; part < HAWK_PART_COUNT; ++part)
      {
         if (hawk_parts[part][4] == 0xffff || hawk_parts[part][4] == HAWK_BEAK ||
             hawk_parts[part][4] == 0x0000)
            continue;
         int left = hawk_parts[part][0] * scale / 100 + HAWK_MASK_WIDTH / 2;
         int top = hawk_parts[part][1] * scale / 100 + HAWK_MASK_HEIGHT / 2;
         int width = hawk_parts[part][2] * scale / 100 + 1;
         int height = hawk_parts[part][3] * scale / 100 + 1;
         for (int row = top; row < top + height; ++row)
            for (int column = left; column < left + width; ++column)
               if (row >= 0 && row < HAWK_MASK_HEIGHT &&
                   column >= 0 && column < HAWK_MASK_WIDTH)
                  hawk_mask[row][column] = 1;
      }
      /* The head shows in the shadow too, just as a rounder lump. */
      for (int row = -48 * scale / 100; row < -26 * scale / 100; ++row)
         for (int column = -8 * scale / 100; column < 8 * scale / 100; ++column)
            hawk_mask[row + HAWK_MASK_HEIGHT / 2][column + HAWK_MASK_WIDTH / 2] = 1;
      for (int row = 0; row < HAWK_MASK_HEIGHT; ++row)
      {
         int y = center_y - HAWK_MASK_HEIGHT / 2 + row;
         if (y < BOARD_Y || y >= BOARD_Y + BOARD_HEIGHT)
            continue;
         for (int column = 0; column < HAWK_MASK_WIDTH; ++column)
         {
            int x = center_x - HAWK_MASK_WIDTH / 2 + column;
            if (!hawk_mask[row][column] || x < BOARD_X || x >= BOARD_X + BOARD_WIDTH)
               continue;
            uint16_t *pixel = drawing_frame + y * SCREEN_WIDTH + x;
            *pixel = (uint16_t)((*pixel >> 1) & 0x7bef);
         }
      }
      return;
   }
   for (unsigned part = 0; part < HAWK_PART_COUNT; ++part)
      fill(center_x + hawk_parts[part][0] * scale / 100,
           center_y + hawk_parts[part][1] * scale / 100,
           hawk_parts[part][2] * scale / 100 + 1,
           hawk_parts[part][3] * scale / 100 + 1, (uint16_t)hawk_parts[part][4]);
   if (talons)
      for (unsigned part = 0; part < sizeof(hawk_talons) / sizeof(hawk_talons[0]); ++part)
         fill(center_x + hawk_talons[part][0] * scale / 100,
              center_y + hawk_talons[part][1] * scale / 100,
              hawk_talons[part][2] * scale / 100 + 1,
              hawk_talons[part][3] * scale / 100 + 1, (uint16_t)hawk_talons[part][4]);
}

/* Unit circle in 16 steps, times 1000, for the circling shadow. */
static const int16_t circle_x[16] = {
   1000, 924, 707, 383, 0, -383, -707, -924, -1000, -924, -707, -383, 0, 383, 707, 924
};
static const int16_t circle_y[16] = {
   0, 383, 707, 924, 1000, 924, 707, 383, 0, -383, -707, -924, -1000, -924, -707, -383
};

/* Screen position of the circling hawk's shadow: a 190-pixel-radius circle
   around the snake's head, at the game's current hawk_angle (16 steps). */
static int hawk_shadow_x(void)
{
   return BOARD_X + game.body[0].x * CELL_SIZE + CELL_SIZE / 2 +
      circle_x[game.hawk_angle % 16] * 190 / 1000;
}

/* The y half of the same circle. */
static int hawk_shadow_y(void)
{
   return BOARD_Y + game.body[0].y * CELL_SIZE + CELL_SIZE / 2 +
      circle_y[game.hawk_angle % 16] * 190 / 1000;
}

/* Everything hawk-related for one frame: the circling shadow, the dive
   warning (a flashing 3x3-cell target with the shadow growing from 100% to
   150% as the dive goes on), the hawk carrying the snake away after a
   catch, or climbing away for 36 frames after a miss. */
static void draw_hawk_scene(void)
{
   int target_x = BOARD_X + game.hawk_target.x * CELL_SIZE + CELL_SIZE / 2;
   int target_y = BOARD_Y + game.hawk_target.y * CELL_SIZE + CELL_SIZE / 2;
   if (game.hawk_phase == SNAKE_HAWK_CIRCLING && game.phase == SNAKE_PLAYING)
      draw_hawk(hawk_shadow_x(), hawk_shadow_y(), 100, true, false);
   else if (game.hawk_phase == SNAKE_HAWK_DIVING && game.phase == SNAKE_PLAYING)
   {
      /* Flashing red target square; the shadow grows as the hawk drops. */
      uint16_t red = game.hawk_steps_left % 2 ? 0xf800 : 0xfa08;
      int left = target_x - CELL_SIZE * 3 / 2;
      int top = target_y - CELL_SIZE * 3 / 2;
      outline(left, top, CELL_SIZE * 3, CELL_SIZE * 3, 5, red);
      for (int corner = 0; corner < 4; ++corner)
      {
         int cx = corner & 1 ? left + CELL_SIZE * 3 - 22 : left;
         int cy = corner & 2 ? top + CELL_SIZE * 3 - 22 : top;
         fill(cx, cy, 22, 22, red);
      }
      unsigned dive = game.hawk_dive_steps ? game.hawk_dive_steps : 1;
      int scale = 100 + (int)((dive - game.hawk_steps_left) * 50 / dive);
      draw_hawk(target_x, target_y, scale, true, false);
   }
   if (hawk_has_snake && hawk_carry_frames)
      draw_hawk(target_x, target_y - snake_lift, 120, false, true);
   else if (hawk_flyoff_frames)
   {
      /* Climbing away after a miss. */
      int rise = (36 - (int)hawk_flyoff_frames) * 14;
      draw_hawk(target_x, target_y - rise, 110 - (36 - (int)hawk_flyoff_frames), false, true);
   }
}

/* Draws the snake's head from head_animation (see compute_head_animation):
   tongue, crown, snout with nostrils or an open mouth, and big eyes that
   are open, blinking, or crossed out after a crash. */
static void draw_head(void)
{
   struct snake_point point = game.body[0];
   int x = BOARD_X + point.x * CELL_SIZE;
   int y = BOARD_Y + point.y * CELL_SIZE - snake_lift;
   enum snake_direction d = game.direction;
   unsigned eyes = head_animation & 3;
   int tongue = (int)((head_animation >> 2) & 7) * 4;
   bool mouth = (head_animation >> 5) & 1;

   /* Forked tongue first, so the snout covers its root. */
   if (tongue)
   {
      head_rect(x, y, d, 44, 24, 12 + tongue, 6, SNAKE_TONGUE);
      head_rect(x, y, d, 53 + tongue, 20, 5, 5, SNAKE_TONGUE);
      head_rect(x, y, d, 53 + tongue, 29, 5, 5, SNAKE_TONGUE);
   }

   /* Lime crown and a yellow snout that narrows toward the front. */
   head_rect(x, y, d, 0, 3, 30, 48, SNAKE_LIME);
   head_rect(x, y, d, 6, 18, 14, 18, SNAKE_LIME_LIGHT);
   head_rect(x, y, d, 30, 5, 14, 44, SNAKE_YELLOW);
   head_rect(x, y, d, 44, 8, 4, 38, SNAKE_YELLOW);
   head_rect(x, y, d, 48, 13, 3, 28, SNAKE_YELLOW);
   if (mouth)
   {
      head_rect(x, y, d, 46, 15, 5, 24, SNAKE_MOUTH);
      head_rect(x, y, d, 46, 23, 5, 8, SNAKE_TONGUE);
   }
   else
   {
      head_rect(x, y, d, 44, 19, 3, 4, SNAKE_NOSTRIL);
      head_rect(x, y, d, 44, 31, 3, 4, SNAKE_NOSTRIL);
   }

   /* Big round eyes that bulge past the sides of the head. */
   for (int eye = 0; eye < 2; ++eye)
   {
      int side = eye ? 35 : 3;
      head_rect(x, y, d, 22, side + 2, 18, 12, 0xffff);
      head_rect(x, y, d, 24, side, 14, 16, 0xffff);
      if (eyes == 1)
      {
         head_rect(x, y, d, 22, side + 2, 18, 12, SNAKE_LIME);
         head_rect(x, y, d, 24, side, 14, 16, SNAKE_LIME);
         head_rect(x, y, d, 29, side + 1, 3, 14, SNAKE_LID_LINE);
      }
      else if (eyes == 2)
         for (int step = 0; step < 10; ++step)
         {
            head_rect(x, y, d, 26 + step, side + 3 + step, 2, 2, 0x0000);
            head_rect(x, y, d, 26 + step, side + 12 - step, 2, 2, 0x0000);
         }
      else
      {
         head_rect(x, y, d, 28, side + 3, 10, 10, SNAKE_IRIS);
         head_rect(x, y, d, 30, side + 5, 7, 6, 0x0000);
         head_rect(x, y, d, 33, side + 6, 3, 3, 0xffff);
      }
   }
}

/* A filled isosceles triangle, 3 pixels wider per row; the arrows above
   and below the selected letter on the name-entry screen. */
static void triangle(int center_x, int y, int height, bool pointing_up, uint16_t color)
{
   for (int row = 0; row < height; ++row)
   {
      int half = pointing_up ? row : height - 1 - row;
      fill(center_x - half * 3 / 2, y + row, half * 3 + 1, 1, color);
   }
}

/* The initials-entry box: "NEW HIGH SCORE" (first place, rainbow) or
   "TOP TEN PLACE n", the score, and three letter slots with the selected
   one highlighted. The border colour cycles every 8 frames; the confetti
   update keeps the screen redrawing while this is shown. */
static void draw_name_entry(void)
{
   static const uint16_t rainbow[] = {
      0xf800, 0xfc00, 0xffe0, 0x07e0, 0x07ff, 0xf81f
   };
   fill(75, 640, 930, 620, 0x0000);
   outline(75, 640, 930, 620, 9, rainbow[(frame_count / 8) % 6]);
   if (entry_rank <= 0)
      banner_text("NEW HIGH SCORE", 672, 66, 100, rainbow[(frame_count / 8 + 2) % 6]);
   else
   {
      char title[] = "TOP TEN  PLACE 00";
      snake_decimal(title + 15, (unsigned)entry_rank + 1, 2);
      /* Drop the leading zero of places 2-9. */
      if (title[15] == '0')
      {
         title[15] = title[16];
         title[16] = 0;
      }
      banner_text(title, 672, 62, 94, 0x07ff);
   }
   char score[6];
   snake_score_text(score, last_score);
   banner_text(score, 782, 60, 90, 0xffe0);

   for (unsigned slot = 0; slot < NAME_LENGTH; ++slot)
   {
      int center = SCREEN_WIDTH / 2 + ((int)slot - (int)(NAME_LENGTH / 2)) * 220;
      bool selected = slot == entry_cursor;
      uint16_t color = selected ? 0xffe0 : 0xffff;
      fill(center - 85, 935, 170, 200, selected ? 0x2104 : 0x1082);
      outline(center - 85, 935, 170, 200, selected ? 7 : 3,
              selected ? 0xffe0 : 0x8410);
      char letter[2] = {entry_name[slot], 0};
      int width = snake_smooth_advance(letter[0], 120);
      smooth_text(letter, center - width / 2 + 5, 960, 120, 150, color);
      if (selected)
      {
         triangle(center, 895, 26, true, 0xffe0);
         triangle(center, 1150, 26, false, 0xffe0);
      }
   }
   banner_text("STICK SPELLS   START SAVES", 1195, 38, 54, 0x8410);
}

/* The DIP switch page, in the game's own style: one row per setting with
   ON/OFF, then EXIT (row SETTING_COUNT). */
static void draw_settings(void)
{
   static const char *names[SETTING_COUNT] = {
      "HAWK", "MOUSE", "SLOW SIGNS", "MOVE SOUND", "ATTRACT MUSIC"
   };
   fill(75, 560, 930, 820, 0x0000);
   outline(75, 560, 930, 820, 9, 0x07ff);
   banner_text("DIP SWITCHES", 590, 72, 100, 0x07ff);
   for (unsigned row = 0; row <= SETTING_COUNT; ++row)
   {
      int y = 730 + (int)row * 90;
      bool selected = row == settings_cursor;
      if (selected)
      {
         fill(110, y - 10, 860, 80, 0x2104);
         outline(110, y - 10, 860, 80, 4, 0xffe0);
      }
      if (row == SETTING_COUNT)
      {
         banner_text("EXIT", y, 50, 64, selected ? 0xffe0 : 0xffff);
         continue;
      }
      smooth_text(names[row], 150, y, 46, 62, selected ? 0xffe0 : 0xffff);
      bool on = setting_on(row);
      smooth_text(on ? "ON" : "OFF", on ? 840 : 820, y, 46, 62, on ? 0x07e0 : 0xf800);
   }
   banner_text("STICK PICKS  ANY BUTTON FLIPS OR EXITS", 1300, 30, 44, 0x8410);
}

/* The top-ten table for the attract screen. Gold, silver and bronze for
   the first three places; the newest entry blinks in green; empty places
   are grey dashes. */
static void draw_high_score_table(uint16_t border, bool blink_on)
{
   static const uint16_t place_colors[3] = { 0xffe0, 0xc618, 0xfc00 };
   fill(75, 560, 930, 860, 0x0000);
   outline(75, 560, 930, 860, 9, border);
   banner_text("HIGH SCORES", 590, 72, 100, 0x07ff);
   for (int rank = 0; rank < HIGH_SCORE_COUNT; ++rank)
   {
      int y = 720 + rank * 64;
      const struct high_score_entry *entry = &high_scores[rank];
      bool newest = rank == newest_rank;
      if (newest && !blink_on)
         continue;
      uint16_t color = newest ? 0x07e0 : rank < 3 ? place_colors[rank] : 0xffff;
      if (!entry->score)
         color = 0x4208;
      char place[3] = { 0, 0, 0 };
      snake_decimal(place, (unsigned)rank + 1, 2);
      smooth_text(place[0] == '0' ? place + 1 : place,
                  place[0] == '0' ? 175 : 150, y, 42, 56, color);
      if (!entry->score)
      {
         /* Empty place: dashes (the smooth font has no '-'). */
         for (int dash = 0; dash < 3; ++dash)
            fill(300 + dash * 34, y + 26, 24, 6, color);
         for (int dash = 0; dash < 4; ++dash)
            fill(900 - 4 * 34 + dash * 34, y + 26, 24, 6, color);
         continue;
      }
      char name[4] = { 0, 0, 0, 0 };
      for (unsigned index = 0; index < NAME_LENGTH; ++index)
         name[index] = entry->name[index];
      smooth_text(name, 300, y, 42, 56, color);
      char digits[6];
      snake_score_text(digits, entry->score);
      int width = 0;
      for (unsigned index = 0; digits[index]; ++index)
         width += snake_smooth_advance(digits[index], 42);
      smooth_text(digits, 900 - width, y, 42, 56, color);
   }
   if (blink_on)
      banner_text("PRESS START", 1360, 46, 64, 0xffe0);
}

/* The box in the middle of the attract screen, cycling every 7 s through
   three pages: the title page (PRESS START and the high score), the
   top-ten table, and a how-to-play page listing only the enabled features.
   The border colour cycles every 10 frames; PRESS START blinks every 30. */
static void draw_attract_panel(void)
{
   static const uint16_t rainbow[] = {
      0xf800, 0xfc00, 0xffe0, 0x07e0, 0x07ff, 0xf81f
   };
   uint16_t border = rainbow[(attract_frames / 10) % 6];
   bool blink_on = (attract_frames / 30) % 2 == 0;
   unsigned panel = (attract_frames / ATTRACT_PANEL_FRAMES) % 3;
   if (panel == 1)
   {
      draw_high_score_table(border, blink_on);
      return;
   }
   bool how_to = panel == 2;
   if (!how_to)
   {
      fill(75, 700, 930, 520, 0x0000);
      outline(75, 700, 930, 520, 9, border);
      banner_text("ALP SNAKE", 735, 104, 140, 0x7e43);
      if (blink_on)
         banner_text("PRESS START", 900, 72, 100, 0xffe0);
      banner_text("HIGH SCORE", 1030, 46, 64, 0x07ff);
      if (game.best)
      {
         /* "ABC   12340": initials (or a space), three spaces, score. */
         char line[] = "XXX  00000";
         char digits[6];
         snake_score_text(digits, game.best);
         unsigned at = 0;
         if (best_name[0])
            for (; at < NAME_LENGTH; ++at)
               line[at] = best_name[at];
         else
            line[at++] = ' ';
         line[at++] = ' ';
         line[at++] = ' ';
         for (unsigned index = 0; digits[index]; ++index)
            line[at++] = digits[index];
         line[at] = 0;
         banner_text(line, 1105, 60, 84, 0x07e0);
      }
      else
         banner_text("BE THE FIRST", 1110, 52, 74, 0x8410);
      return;
   }

   fill(75, 600, 930, 720, 0x0000);
   outline(75, 600, 930, 720, 9, border);
   banner_text("HOW TO PLAY", 630, 70, 96, 0x07ff);
   int y = 760;
   int icon_x = 140;
   int text_x = 260;
   /* Food */
   fill(icon_x + 8, y + 8, 38, 38, 0xf800);
   fill(icon_x + 16, y + 16, 22, 22, 0xffe0);
   smooth_text("FOOD", text_x, y + 2, 44, 60, 0xffff);
   smooth_text("10", text_x + 470, y + 2, 44, 60, 0xffe0);
   y += 105;
   if (configured_features & SNAKE_FEATURE_MOUSE)
   {
      draw_mouse_at(icon_x, y, SNAKE_RIGHT, (attract_frames / 12) % 2);
      smooth_text("MOUSE", text_x, y - 14, 44, 60, 0xffff);
      smooth_text("20", text_x + 470, y - 14, 44, 60, 0xffe0);
      smooth_text("SHRINKS THE SNAKE", text_x, y + 42, 30, 40, 0x8410);
      y += 120;
   }
   if (configured_features & SNAKE_FEATURE_SIGNS)
   {
      draw_sign_at(icon_x, y);
      smooth_text("SLOW SIGN", text_x, y - 14, 44, 60, 0xffff);
      smooth_text("SLOWS YOU DOWN", text_x, y + 42, 30, 40, 0x8410);
      y += 120;
   }
   if (configured_features & SNAKE_FEATURE_HAWK)
   {
      draw_hawk(icon_x + 27, y + 24, 55, false, false);
      smooth_text("HAWK", text_x, y - 14, 44, 60, 0xffff);
      smooth_text("WATCH THE RED TARGET", text_x, y + 42, 30, 40, 0x8410);
      smooth_text("AND TURN AWAY", text_x, y + 82, 30, 40, 0x8410);
   }
   if (blink_on)
      banner_text("PRESS START", 1240, 50, 70, 0xffe0);
}

/* Paints a complete frame into the back buffer and then flips buffers so
   retro_run presents it. Drawing is back to front: background and header
   with scores, board frame and grid, food, sign, mouse, snake (tail, body
   from tail to neck, head; lifted by snake_lift while the hawk carries it),
   hawk, popup, then whichever box covers the board (DIP switches, attract
   panel, name entry or the Ready/Paused/Game Over box), confetti, and the
   bottom line (diagnostics or the high-score holder). Called only when
   frame_dirty is set; a full redraw is a couple of million pixel writes. */
static void draw_game(void)
{
   drawing_frame = frame_buffers[front_buffer ^ 1u];
   fill(0, 0, SCREEN_WIDTH, SCREEN_HEIGHT, 0x0843);
   fill(0, 0, SCREEN_WIDTH, 24, 0x07ff);
   fill(0, SCREEN_HEIGHT - 24, SCREEN_WIDTH, 24, 0x07ff);
   banner_text("ALP SNAKE", 38, 92, 116, 0x07ff);
   smooth_text("SCORE", 69, 157, 48, 74, 0xffff);
   smooth_text("BEST", 555, 157, 48, 74, 0xffff);
   char score[6];
   char best[6];
   snake_score_text(score, attract ? last_score : game.score);
   snake_score_text(best, game.best);
   smooth_text(score, 321, 157, 52, 74, 0xffe0);
   smooth_text(best, 768, 157, 52, 74, 0x07e0);

   /* Board: cyan frame, dark border, background (972 x 1512 = the board),
      then 2-pixel grid lines between cells. */
   fill(BOARD_X - 12, BOARD_Y - 12, 996, 1536, 0x07ff);
   fill(BOARD_X - 6, BOARD_Y - 6, 984, 1524, 0x1085);
   fill(BOARD_X, BOARD_Y, 972, 1512, 0x0843);
   for (int column = 1; column < SNAKE_COLUMNS; ++column)
      fill(BOARD_X + column * CELL_SIZE - 1, BOARD_Y, 2, 1512, 0x10a4);
   for (int row = 1; row < SNAKE_ROWS; ++row)
      fill(BOARD_X, BOARD_Y + row * CELL_SIZE - 1, 972, 2, 0x10a4);

   int food_x = BOARD_X + game.food.x * CELL_SIZE;
   int food_y = BOARD_Y + game.food.y * CELL_SIZE;
   fill(food_x + 8, food_y + 8, 38, 38, 0xf800);
   fill(food_x + 16, food_y + 16, 22, 22, 0xffe0);
   draw_sign();
   draw_mouse();
   bool snake_gone = hawk_has_snake && !hawk_carry_frames;
   if (game.length >= 2 && !snake_gone)
      draw_tail();
   /* Body segments from the tail end toward the neck (body[1]); body[0] is
      the head and body[length - 1] the tail, drawn separately. Segments
      lifted above the board top are skipped. Alternating shades give the
      snake its banding. */
   for (unsigned segment = snake_gone ? 1 : game.length - 1; segment > 1; --segment)
   {
      struct snake_point point = game.body[segment - 1];
      int x = BOARD_X + point.x * CELL_SIZE;
      int y = BOARD_Y + point.y * CELL_SIZE - snake_lift;
      if (y < BOARD_Y)
         continue;
      uint16_t color = segment % 2 ? SNAKE_LIME : SNAKE_LIME_DARK;
      fill(x + 3, y + 3, 48, 48, color);
      fill(x + 9, y + 9, 36, 36, SNAKE_LIME);
      fill(x + 21, y + 21, 12, 12, SNAKE_LIME_LIGHT);
   }
   if (game.length && !snake_gone)
      draw_head();
   draw_hawk_scene();
   draw_catch_popup();

   if (settings_open)
      draw_settings();
   else if (hawk_carry_frames)
      ;  /* Let the hawk's exit play out before any box covers the board. */
   else if (attract && !show_diagnostics)
      draw_attract_panel();
   else if (entering_name)
      draw_name_entry();
   else if (game.phase != SNAKE_PLAYING || attract)
   {
      fill(105, 790, 870, 330, 0x0000);
      outline(105, 790, 870, 330, 9, 0xffe0);
      /* Attract only gets here while diagnostics are shown: the box then
         lists the current settings. */
      if (attract)
         banner_text("SETTINGS", 825, 84, 134, 0x07ff);
      else if (game.phase == SNAKE_READY)
         banner_text("READY", 825, 84, 134, 0x07ff);
      else if (game.phase == SNAKE_PAUSED)
         banner_text("PAUSED", 825, 84, 134, 0xffe0);
      else if (game.phase == SNAKE_WON)
         banner_text("YOU WIN", 825, 84, 134, 0x07e0);
      else if (last_game_record)
         banner_text("NEW BEST", 825, 84, 134, 0x07e0);
      else if (last_game_rank > 0)
         banner_text("TOP TEN", 825, 84, 134, 0x07ff);
      else
         banner_text("GAME OVER", 825, 80, 134, 0xf800);
      if (show_diagnostics)
      {
         /* What the game is using, and whether the cabinet supplied it. */
         /* Patch "OFF" into "ON " in place: index 6-7 and 17-18 are the
            last two letters of each OFF ("WAKA" is the move sound). */
         char line[] = "HAWK OFF  MOUSE OFF";
         line[6] = configured_features & SNAKE_FEATURE_HAWK ? 'N' : 'F';
         line[7] = configured_features & SNAKE_FEATURE_HAWK ? ' ' : 'F';
         line[17] = configured_features & SNAKE_FEATURE_MOUSE ? 'N' : 'F';
         line[18] = configured_features & SNAKE_FEATURE_MOUSE ? ' ' : 'F';
         banner_text(line, 968, 40, 58, 0xffff);
         char second[] = "SIGNS OFF  WAKA OFF";
         second[7] = configured_features & SNAKE_FEATURE_SIGNS ? 'N' : 'F';
         second[8] = configured_features & SNAKE_FEATURE_SIGNS ? ' ' : 'F';
         second[17] = move_sound_enabled ? 'N' : 'F';
         second[18] = move_sound_enabled ? ' ' : 'F';
         banner_text(second, 1030, 40, 58, 0xffff);
         banner_text(options_from_cabinet ? "SET FROM CABINET MENU" : "CABINET MENU NOT SEEN",
                     1078, 30, 40, options_from_cabinet ? 0x07e0 : 0xfc00);
      }
      else
         banner_text("START TO PLAY", 1010, 53, 76, 0xffff);
   }
   draw_confetti();

   if (show_diagnostics)
   {
      /* "FPS 5994" means 59.94 frames per second; GAP is the slowest frame
         and DRAW the slowest picture redraw of the last two seconds, in ms. */
      char line[] = "FPS 0000 SND 000 GAP 000 DRAW 000";
      snake_decimal(line + 4, measured_fps_x100 % 10000, 4);
      snake_decimal(line + 13, audio_accepted_percent > 999 ? 999 :
                    audio_accepted_percent, 3);
      snake_decimal(line + 21, shown_gap_ms > 999 ? 999 : shown_gap_ms, 3);
      snake_decimal(line + 30, shown_draw_ms > 999 ? 999 : shown_draw_ms, 3);
      banner_text(line, 1815, 34, 54, 0xffe0);
   }
   else if (best_name[0])
   {
      char holder[] = "HIGH SCORE BY XXX";
      for (unsigned index = 0; index < NAME_LENGTH; ++index)
         holder[14 + index] = best_name[index];
      banner_text(holder, 1810, 49, 76, 0x07ff);
   }
#ifdef ALP_SNAKE_PROBE
   draw_probe();
#endif
   /* The finished back buffer becomes the one retro_run presents. */
   front_buffer ^= 1u;
   frame_dirty = false;
}

/* ---- 13. Audio clock, diagnostics, keepalive ------------------------- */

/* How many stereo samples this frame should carry: the real time since the
   last frame at 44.1 kHz, so sound stays in step with the cabinet's audio
   clock even when Retroplayer runs slightly faster or slower than 60 Hz. */
static unsigned audio_frames_due(uint64_t now)
{
   if (!now)
      return AUDIO_FRAMES;
   if (!audio_clock_ns || now <= audio_clock_ns ||
       now - audio_clock_ns > RESUME_GAP_NS)
   {
      /* First frame, or back from a pause or stall: start fresh rather than
         flooding the frontend with the missed time. */
      audio_clock_ns = now;
      audio_remainder = 0;
      return AUDIO_FRAMES;
   }
   /* elapsed ns x 44100 / 1e9, carrying the remainder to the next frame so
      no fraction of a sample is ever lost. */
   uint64_t scaled = (now - audio_clock_ns) * 44100u + audio_remainder;
   audio_clock_ns = now;
   uint64_t frames = scaled / 1000000000u;
   audio_remainder = scaled % 1000000000u;
   /* A slow frame (drawing can take the cabinet a while) must not lose
      sound: hand over everything that time covers. Longer gaps than this
      were handled above as a pause. */
   if (frames > AUDIO_MAX_FRAMES)
   {
      frames = AUDIO_MAX_FRAMES;
      audio_remainder = 0;
   }
   return (unsigned)frames;
}

/* Diagnostics bookkeeping: every 120 frames (about 2 s) works out the real
   frame rate in hundredths of a frame per second, publishes the worst
   frame gap and drawing time of that window, and the share of offered
   audio the frontend accepted. A window older than 5 s (after a pause) is
   discarded and restarted. */
static void measure_frame_rate(uint64_t now)
{
   if (!now)
      return;
   if (!diagnostic_window_ns || now - diagnostic_window_ns > 5000000000u)
   {
      diagnostic_window_ns = now;
      diagnostic_frames = 0;
      diagnostic_offered = 0;
      diagnostic_accepted = 0;
      return;
   }
   if (++diagnostic_frames < 120)
      return;
   /* frames x 100 x 1e9 / elapsed ns = frames per second x 100. */
   measured_fps_x100 = (unsigned)((uint64_t)diagnostic_frames * 100000000000u /
                                  (now - diagnostic_window_ns));
   shown_gap_ms = worst_gap_ms;
   shown_draw_ms = worst_draw_ms;
   worst_gap_ms = 0;
   worst_draw_ms = 0;
   audio_accepted_percent = diagnostic_offered ?
      (unsigned)(diagnostic_accepted * 100 / diagnostic_offered) : 100;
   diagnostic_window_ns = now;
   diagnostic_frames = 0;
   diagnostic_offered = 0;
   diagnostic_accepted = 0;
   if (show_diagnostics)
      frame_dirty = true;
}

/* Produces and delivers this frame's sound. The amount is set by the real
   time since the last frame (audio_frames_due), not a fixed 735 samples,
   which fixed earlier choppy and drifting audio when frames ran late. Also
   records the frame gap for diagnostics. Prefers the batch callback (one
   call for the whole buffer) and counts how much of it was accepted. */
static void generate_audio(void)
{
   uint64_t now = monotonic_nanoseconds();
   if (now && previous_frame_ns && now - previous_frame_ns < RESUME_GAP_NS)
   {
      unsigned gap = (unsigned)((now - previous_frame_ns) / 1000000u);
      if (gap > worst_gap_ms)
         worst_gap_ms = gap;
   }
   previous_frame_ns = now;
   measure_frame_rate(now);
   unsigned frames = audio_frames_due(now);
   snake_audio_render(audio, frames);
   if (audio_batch_callback)
   {
      size_t accepted = audio_batch_callback(audio, frames);
      diagnostic_offered += frames;
      diagnostic_accepted += accepted > frames ? frames : accepted;
   }
   else if (audio_callback)
      for (unsigned sample = 0; sample < frames; ++sample)
         audio_callback(audio[sample * 2], audio[sample * 2 + 1]);
}

/* Every 1200 frames (~20 s) pushes a fake F24 key press and release into
   SDL's event queue. The cabinet firmware dims or idles after a while
   without input events; F24 does nothing in any game but counts as
   activity, so attract mode can run forever without the cabinet idling.
   The array is laid out like SDL2's SDL_KeyboardEvent inside the 56-byte
   SDL_Event union (14 x 32 bits): [0] type 0x300 SDL_KEYDOWN / 0x301
   SDL_KEYUP, [3] low byte = state (1 pressed), [4] scancode 115 =
   SDL_SCANCODE_F24, [5] keycode 0x40000073 = SDLK_F24. Built by hand
   because the core has no SDL headers. */
static void keepalive(void)
{
   if (frame_count % 1200)
      return;
   uint32_t event[14] = {0};
   event[0] = 0x300;
   event[3] = 1;
   event[4] = 115;
   event[5] = 0x40000073;
   SDL_PushEvent(event);
   event[0] = 0x301;
   event[3] = 0;
   SDL_PushEvent(event);
}

/* libretro: the first call Retroplayer makes. The environment callback is
   how the core asks the frontend for things (variables, pixel format...). */
void retro_set_environment(retro_environment_t callback)
{
   environment_callback = callback;
   PROBE("retro_set_environment");
}

/* ---- 14. Settings and the cabinet-menu handshake --------------------- */

/* Whether a DIP switch is on. */
static bool setting_on(unsigned setting)
{
   return (settings_bits >> setting) & 1u;
}

/* Puts the DIP switch settings into effect: game features for the next
   (or current) real game, the move sound and the attract music. While in
   attract the demo keeps its own features (mice only). */
static void apply_settings(void)
{
   unsigned features = 0;
   if (setting_on(SETTING_MOUSE))
      features |= SNAKE_FEATURE_MOUSE;
   if (setting_on(SETTING_SIGNS))
      features |= SNAKE_FEATURE_SIGNS;
   if (setting_on(SETTING_HAWK))
      features |= SNAKE_FEATURE_HAWK;
   configured_features = features;
   if (!attract)
      game.features = features;
   move_sound_enabled = setting_on(SETTING_MOVE_SOUND);
   attract_music_enabled = setting_on(SETTING_MUSIC);
   if (attract)
      snake_audio_music(attract_music_enabled);
   frame_dirty = true;
}

/* Reads save/settings.dat: { magic, bits, ~bits }. Storing the bits and
   their inverse lets a damaged file be detected; anything invalid leaves
   the defaults (everything on). */
static void load_settings(void)
{
   uint32_t record[3];
   long descriptor = system_call(SYS_openat, AT_FDCWD, (long)SETTINGS_PATH, O_RDONLY | O_CLOEXEC, 0, 0, 0);
   if (descriptor < 0)
      return;
   long bytes = system_call(SYS_read, descriptor, (long)record, sizeof(record), 0, 0, 0);
   system_call(SYS_close, descriptor, 0, 0, 0, 0, 0);
   if (bytes == sizeof(record) && record[0] == SETTINGS_MAGIC &&
       record[2] == ~record[1] && record[1] < (1u << SETTING_COUNT))
      settings_bits = record[1];
}

/* Writes settings.dat atomically (temp file, fsync, rename), like the
   high-score table. */
static void save_settings(void)
{
   uint32_t record[3] = { SETTINGS_MAGIC, settings_bits, ~settings_bits };
   long descriptor = system_call(SYS_openat, AT_FDCWD, (long)SETTINGS_TEMP_PATH,
                                 O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644, 0, 0);
   if (descriptor < 0)
      return;
   long bytes = system_call(SYS_write, descriptor, (long)record, sizeof(record), 0, 0, 0);
   long synced = bytes == sizeof(record) ?
      system_call(SYS_fsync, descriptor, 0, 0, 0, 0, 0) : -1;
   system_call(SYS_close, descriptor, 0, 0, 0, 0, 0);
   if (synced != 0 || system_call(SYS_renameat, AT_FDCWD, (long)SETTINGS_TEMP_PATH,
                                  AT_FDCWD, (long)SETTINGS_PATH, 0, 0) != 0)
      system_call(SYS_unlinkat, AT_FDCWD, (long)SETTINGS_TEMP_PATH, 0, 0, 0, 0);
}

/* Opens the DIP switch page, pausing a game in progress. */
static void open_settings(void)
{
   if (settings_open)
      return;
   settings_open = true;
   settings_cursor = 0;
   held_direction = -1;
   /* Pause a game in progress while the switches are open. */
   settings_paused_game = !attract && game.phase == SNAKE_PLAYING;
   if (settings_paused_game)
      snake_pause(&game);
   frame_dirty = true;
}

/* Closes the DIP switch page: saves and applies the switches, resumes a
   game this page paused (snake_pause toggles), and notes the frame so the
   retro_reset that follows a cabinet-menu close can be ignored. */
static void close_settings(void)
{
   if (!settings_open)
      return;
   settings_open = false;
   settings_closed_frame = frame_count;
   save_settings();
   apply_settings();
   if (settings_paused_game && game.phase == SNAKE_PAUSED)
      snake_pause(&game);
   settings_paused_game = false;
   frame_dirty = true;
}

/* Adv. Config in the cabinet menu switches "Display MAME menu" on, and
   back off when that menu is left: open and close the DIP switch page to
   match. */
static void check_display_setup(void)
{
   struct retro_variable variable = {
      option_variables[DISPLAY_SETUP_VARIABLE].key, 0
   };
   if (!environment_callback ||
       !environment_callback(RETRO_ENVIRONMENT_GET_VARIABLE, &variable) ||
       !variable.value)
      return;
   options_from_cabinet = true;
   bool enabled = variable.value[0] == 'e';   /* "enabled" / "disabled" */
   if (enabled == display_setup_enabled)
      return;
   display_setup_enabled = enabled;
   last_display_setup_change = frame_count;
   if (enabled)
      open_settings();
   else
      close_settings();
}

/* Called every frame: asks whether any core variable changed since last
   time (cheap), and only then reads the display-setup variable. */
static void check_options(void)
{
   bool updated = false;
   if (environment_callback &&
       environment_callback(RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE, &updated) &&
       updated)
   {
#ifdef ALP_SNAKE_PROBE
      probe_event("GET_VARIABLE_UPDATE says updated");
#endif
      check_display_setup();
   }
}

#ifdef ALP_SNAKE_PROBE
#define PROBE_VARIABLES (sizeof(option_variables) / sizeof(option_variables[0]) - 1)
static char probe_values[PROBE_VARIABLES][48];
static bool probe_values_known;
static int16_t probe_joypad[2][16];
static int16_t probe_keys[24];
static uint64_t probe_last_run_ns;
/* ---- 15. Probe build: variable and input logging --------------------- */

/* Keyboard keys MAME uses for its menus and coins, as libretro RETROK_
   codes: Tab, Enter, Escape, Space, 1, 2, 5, 6, P, F1-F12, Up, Down, Left.
   (Right, 275, didn't fit in the 24 slots.) */
static const unsigned probe_key_codes[24] = {
   9, 13, 27, 32, 49, 50, 53, 54, 112, 282, 283, 284, 285, 286, 287, 288,
   289, 290, 291, 292, 293, 273, 274, 276
};

/* Logs every setting whose value changed since the last look. */
static void probe_check_variables(void)
{
   for (unsigned index = 0; index < PROBE_VARIABLES; ++index)
   {
      struct retro_variable variable = { option_variables[index].key, 0 };
      bool ok = environment_callback &&
         environment_callback(RETRO_ENVIRONMENT_GET_VARIABLE, &variable);
      const char *value = ok && variable.value ? variable.value : "(none)";
      char *known = probe_values[index];
      bool same = probe_values_known;
      for (unsigned at = 0; same && at < 47; ++at)
      {
         if (known[at] != value[at])
            same = false;
         if (!value[at])
            break;
      }
      if (same)
         continue;
      unsigned at = 0;
      for (; value[at] && at < 47; ++at)
         known[at] = value[at];
      known[at] = 0;
      struct probe_line line;
      probe_begin(&line, "VAR ");
      probe_add(&line, option_variables[index].key);
      probe_add(&line, " = ");
      probe_add(&line, value);
      probe_finish(&line);
      if (index == 5)   /* mame2003-plus_display_setup (DISPLAY_SETUP_VARIABLE) */
         probe_menu_requested = value[0] == 'e';
   }
   probe_values_known = true;
}

/* Logs every change of the 16 joypad buttons on ports 0 and 1 and of the
   keyboard keys above, to learn what the cabinet menu presses (that is how
   Insert Coin = Select and Adv. Config = R3 were found). */
static void probe_check_input(void)
{
   if (!input_state_callback)
      return;
   for (unsigned port = 0; port < 2; ++port)
      for (unsigned id = 0; id < 16; ++id)
      {
         int16_t value = input_state_callback(port, RETRO_DEVICE_JOYPAD, 0, id);
         if (value == probe_joypad[port][id])
            continue;
         probe_joypad[port][id] = value;
         struct probe_line line;
         probe_begin(&line, "JOYPAD port ");
         probe_number(&line, port);
         probe_add(&line, " button ");
         probe_number(&line, id);
         probe_add(&line, value ? " down" : " up");
         probe_finish(&line);
      }
   for (unsigned key = 0; key < 24; ++key)
   {
      int16_t value = input_state_callback(0, RETRO_DEVICE_KEYBOARD, 0,
                                           probe_key_codes[key]);
      if (value == probe_keys[key])
         continue;
      probe_keys[key] = value;
      struct probe_line line;
      probe_begin(&line, "KEY ");
      probe_number(&line, probe_key_codes[key]);
      probe_add(&line, value ? " down" : " up");
      probe_finish(&line);
   }
}

/* Per-frame probe work: logs stalls over 200 ms (e.g. while the cabinet
   menu is open), checks variables every 15 frames and input every frame. */
static void probe_frame(void)
{
   uint64_t now = monotonic_nanoseconds();
   if (probe_last_run_ns && now - probe_last_run_ns > 200000000u)
      probe_event_number("GAP before this frame, ms", (unsigned long)((now - probe_last_run_ns) / 1000000u));
   probe_last_run_ns = now;
   if (frame_count % 15 == 0)
      probe_check_variables();
   probe_check_input();
}

/* Overlays the last five log lines on the board, plus a red banner while
   Retroplayer reports Adv. Config ("Display MAME menu") enabled. */
static void draw_probe(void)
{
   fill(BOARD_X, BOARD_Y, BOARD_WIDTH, 5 * 30 + 16, 0x0000);
   text("PROBE BUILD - LOG IN SAVE/PROBE.LOG", BOARD_X + 8, BOARD_Y + 4, 3, 0x07ff);
   for (unsigned row = 0; row < 5; ++row)
   {
      unsigned index = (probe_recent_next + row) % 5;
      text(probe_recent[index], BOARD_X + 8, BOARD_Y + 34 + row * 26, 3, 0xffe0);
   }
   if (probe_menu_requested)
   {
      fill(140, 1500, 800, 120, 0xf800);
      banner_text("ADV CONFIG SEEN", 1525, 60, 80, 0xffff);
   }
}
#endif

/* ---- 16. libretro entry points --------------------------------------- */

/* Retroplayer hands over its callbacks once, before loading the game. */
void retro_set_video_refresh(retro_video_refresh_t callback) { video_callback = callback; }
void retro_set_audio_sample(retro_audio_sample_t callback) { audio_callback = callback; }
void retro_set_audio_sample_batch(retro_audio_sample_batch_t callback) { audio_batch_callback = callback; }
void retro_set_input_poll(retro_input_poll_t callback) { input_poll_callback = callback; }
void retro_set_input_state(retro_input_state_t callback) { input_state_callback = callback; }
/* Nothing to set up until a game is loaded; the real work is in
   retro_load_game. */
void retro_init(void) { PROBE("retro_init"); }
void retro_deinit(void) { PROBE("retro_deinit"); }
unsigned retro_api_version(void) { PROBE("retro_api_version"); return RETRO_API_VERSION; }

/* libretro: who we are. We copy MAME 2003-Plus so Retroplayer shows its
   full arcade menu (see the file header and DEVELOPER.md section 4). */
void retro_get_system_info(struct retro_system_info *info)
{
   /* Exactly what the cabinet's MAME 2003-Plus core reports, so Retroplayer
      gives ALP Snake the full arcade menu. */
   PROBE("retro_get_system_info");
   /* need_fullpath: Retroplayer passes the path of roms/snake.zip (a text
      placeholder) instead of loading it into memory; block_extract: don't
      unzip it. */
   info->library_name = "MAME 2003-Plus";
   info->library_version = " e3b0d1e3";
   info->valid_extensions = "zip";
   info->need_fullpath = true;
   info->block_extract = true;
}

/* Video and audio format: a fixed 1080x1920 portrait picture (9:16) at
   60 fps, stereo sound at 44.1 kHz. */
void retro_get_system_av_info(struct retro_system_av_info *info)
{
   info->geometry.base_width = SCREEN_WIDTH;
   info->geometry.base_height = SCREEN_HEIGHT;
   info->geometry.max_width = SCREEN_WIDTH;
   info->geometry.max_height = SCREEN_HEIGHT;
   info->geometry.aspect_ratio = 9.0f / 16.0f;
   info->timing.fps = 60.0;
   info->timing.sample_rate = 44100.0;
}

/* Retroplayer tells us which controller type is in each port. There is only
   one kind, so this is just logged in the probe build. */
void retro_set_controller_port_device(unsigned port, unsigned device)
{
#ifdef ALP_SNAKE_PROBE
   struct probe_line line;
   probe_begin(&line, "retro_set_controller_port_device port ");
   probe_number(&line, port);
   probe_add(&line, " device ");
   probe_number(&line, device);
   probe_finish(&line);
#endif
   (void)port;
   (void)device;
}

/* libretro: "reset the machine". Sent every time the cabinet menu closes. */
void retro_reset(void)
{
   PROBE("retro_reset");
   /* Retroplayer restarts the game when Adv. Config closes (MAME needs that
      after DIP switch changes). ALP Snake applies settings live, so don't
      throw away a game for it; the attract screen has nothing to reset.
      600 frames is a 10 s grace period after the page or variable last
      changed. A reset at any other time restarts the game. */
   if (settings_open || display_setup_enabled ||
       frame_count - settings_closed_frame < 600 ||
       frame_count - last_display_setup_change < 600 || attract)
      return;
   if (entering_name)
      finish_name_entry();
   start_real_game();
   move_frames = 0;
   next_move_ns = 0;
   last_run_ns = 0;
}

/* libretro: called once per display frame (~60 times a second). The order:
     1. check the cabinet menu's variables (DIP switch page open/close);
     2. look for the trackball, read all input;
     3. advance the demo (attract) or the real game when its move is due;
     4. turn game events into sounds, popups and hawk animations;
     5. confetti, timers, the return to attract after a game;
     6. redraw only if something changed (frame_dirty), timing the draw;
     7. update the backglass state file (10 times a second or on change);
     8. present the front buffer, generate audio, maybe send the keepalive.
   Presenting the same buffer again when nothing changed is cheap, which
   keeps most frames well inside the 16.7 ms budget. */
void retro_run(void)
{
   ++frame_count;
   check_options();
#ifdef ALP_SNAKE_PROBE
   probe_frame();
#endif
   find_trackball();
   poll_controls();
   if (attract)
   {
      update_attract();
      move_frames = 0;
      next_move_ns = 0;
      last_run_ns = 0;
   }
   else if (game.phase == SNAKE_PLAYING)
   {
      uint64_t now = monotonic_nanoseconds();
      bool advance = false;
      if (now)
      {
         /* After a frontend pause or long stall, give the player a full
            interval before the next move instead of moving immediately. */
         if (!next_move_ns ||
             (last_run_ns && now - last_run_ns > RESUME_GAP_NS))
            next_move_ns = now + movement_interval_ns();
         last_run_ns = now;
         /* Move on the frame nearest the deadline: up to half a frame
            early is better than almost a whole frame late. */
         advance = now + HALF_FRAME_NS >= next_move_ns;
      }
      else
      {
         /* No clock: fall back to counting frames. */
         next_move_ns = 0;
         last_run_ns = 0;
         if (++move_frames >= movement_period())
         {
            move_frames = 0;
            advance = true;
         }
      }
      if (advance)
      {
         uint64_t deadline = next_move_ns;
         unsigned previous_period = movement_period();
         unsigned previous_best = game.best;
         enum snake_event event = snake_step(&game);
         if (now)
         {
            /* Schedule from the previous deadline, not from now, so small
               frame jitter doesn't accumulate. Restart from now if the
               speed changed or we have fallen more than half an interval
               behind, rather than firing several catch-up moves. */
            uint64_t interval = movement_interval_ns();
            next_move_ns = deadline + interval;
            if (previous_period != movement_period() ||
                next_move_ns <= now || next_move_ns - now < interval / 2)
               next_move_ns = now + interval;
         }
         if (game.best > previous_best)
            high_score_dirty = true;
         bool ended = event == SNAKE_EVENT_DIE || event == SNAKE_EVENT_WIN;
         int rank = ended ? high_score_rank(game.score) : -1;
         /* "record" means a new number one: confetti and fanfare. */
         bool record = rank == 0;
         if (ended)
         {
            last_score = game.score;
            high_score_dirty = false;
            if (rank >= 0)
               start_name_entry(rank);
            else
               sync_best_from_table();
         }
         frame_dirty = true;
         if (event == SNAKE_EVENT_MOVE && move_sound_enabled)
            /* variant: bit 0 alternates the "waka", bits 1+ = speed level. */
            snake_audio_play(SNAKE_SOUND_MOVE, game.body[0].x,
                             (game.steps & 1) | (8 - movement_period()) << 1);
         else if (event == SNAKE_EVENT_EAT)
            play_sound(SNAKE_SOUND_EAT);
         else if (event == SNAKE_EVENT_WIN)
            play_sound(SNAKE_SOUND_WIN);
         else if (event == SNAKE_EVENT_DIE && record)
            play_sound(SNAKE_SOUND_HIGH_SCORE);
         else if (event == SNAKE_EVENT_DIE && game.hawk_event != SNAKE_HAWK_CAUGHT)
            play_sound(SNAKE_SOUND_DIE);
         if (game.mouse_event == SNAKE_MOUSE_APPEARED)
            snake_audio_play(SNAKE_SOUND_MOUSE, game.mouse.x, 0);
         if (game.sign_event == SNAKE_SIGN_APPEARED)
            snake_audio_play(SNAKE_SOUND_SIGN, game.sign.x, 0);
         else if (game.sign_event == SNAKE_SIGN_EATEN)
         {
            play_sound(SNAKE_SOUND_SLOW_DOWN);
            popup_is_slow = true;
            catch_popup_frames = 75;   /* 1.25 s */
            catch_popup_x = BOARD_X + game.body[0].x * CELL_SIZE + 10;
            catch_popup_y = BOARD_Y + game.body[0].y * CELL_SIZE - 62;
         }
         if (game.hawk_event == SNAKE_HAWK_ARRIVED)
            snake_audio_play(SNAKE_SOUND_HAWK_SCREECH,
                             (hawk_shadow_x() - BOARD_X) / CELL_SIZE, 0);
         else if (game.hawk_event == SNAKE_HAWK_DIVES)
            snake_audio_play(SNAKE_SOUND_HAWK_DIVE, game.hawk_target.x, 0);
         else if (game.hawk_event == SNAKE_HAWK_MISSED)
         {
            snake_audio_play(SNAKE_SOUND_HAWK_MISS, game.hawk_target.x, 0);
            hawk_flyoff_frames = 36;
            popup_is_slow = false;
            popup_points = "50";
            catch_popup_frames = 75;
            catch_popup_x = BOARD_X + game.body[0].x * CELL_SIZE + 10;
            catch_popup_y = BOARD_Y + game.body[0].y * CELL_SIZE - 62;
         }
         else if (game.hawk_event == SNAKE_HAWK_CAUGHT)
         {
            hawk_has_snake = true;
            hawk_carry_frames = HAWK_CARRY_FRAMES;
            snake_lift = 0;
            /* A new record keeps its fanfare; otherwise the hawk's own sound. */
            snake_audio_play(record ? SNAKE_SOUND_HAWK_SCREECH : SNAKE_SOUND_HAWK_CATCH,
                             game.hawk_target.x, 0);
         }
         if (game.mouse_event == SNAKE_MOUSE_CAUGHT)
         {
            play_sound(SNAKE_SOUND_MOUSE_CAUGHT);
            popup_is_slow = false;
            popup_points = "20";
            catch_popup_frames = 60;
            catch_popup_x = BOARD_X + game.body[0].x * CELL_SIZE + 10;
            catch_popup_y = BOARD_Y + game.body[0].y * CELL_SIZE - 62;
         }
      }
   }
   else
   {
      move_frames = 0;
      next_move_ns = 0;
      last_run_ns = 0;
   }

   update_confetti();
   /* A few seconds after a game ends, go back to the attract screen. */
   if (!attract && (game.phase == SNAKE_GAME_OVER || game.phase == SNAKE_WON) &&
       !entering_name && !hawk_carry_frames && !confetti_active)
   {
      if (++game_over_frames >= GAME_OVER_TO_ATTRACT)
         enter_attract();
   }
   else
      game_over_frames = 0;
   if (catch_popup_frames && --catch_popup_frames == 0)
      frame_dirty = true;
   if (hawk_flyoff_frames)
   {
      --hawk_flyoff_frames;
      frame_dirty = true;
   }
   if (hawk_carry_frames)
   {
      /* A short pause on the grab, then an accelerating climb. */
      unsigned elapsed = HAWK_CARRY_FRAMES - --hawk_carry_frames;
      /* Quadratic: (90 - 20)^2 / 3 = about 1630 px, well off the top. */
      snake_lift = elapsed < 20 ? 0 : (int)((elapsed - 20) * (elapsed - 20) / 3);
      frame_dirty = true;
   }
   unsigned animation = compute_head_animation();
   if (animation != head_animation)
   {
      head_animation = animation;
      frame_dirty = true;
   }
   /* Remember whether anything changed this frame before draw_game() clears
      the flag, so the backglass hears about it on the same frame. */
   bool changed_this_frame = frame_dirty;
   if (frame_dirty)
   {
      uint64_t draw_start = monotonic_nanoseconds();
      draw_game();
      uint64_t draw_end = monotonic_nanoseconds();
      if (draw_start && draw_end > draw_start)
      {
         unsigned draw_ms = (unsigned)((draw_end - draw_start) / 1000000u);
         if (draw_ms > worst_draw_ms)
            worst_draw_ms = draw_ms;
      }
   }
   /* Tell the backglass helper about the new state when something changed,
      and every 6th frame regardless (10 times a second) as a safety net. The
      write is a tiny file in /tmp (RAM), so this is cheap. */
   if (frame_count % 6 == 0 || changed_this_frame)
      write_backglass_state();
   /* Present the finished buffer; pitch is the bytes per row. */
   if (video_callback)
      video_callback(frame_buffers[front_buffer], SCREEN_WIDTH, SCREEN_HEIGHT,
                     SCREEN_WIDTH * sizeof(uint16_t));
   generate_audio();
   keepalive();
}

/* No save states: Save Slots don't work on the cabinet even for MAME games,
   and reporting none spares Retroplayer copying the game every few frames. */
size_t retro_serialize_size(void) { return 0; }
bool retro_serialize(void *data, size_t size) { (void)data; (void)size; return false; }
bool retro_unserialize(const void *data, size_t size) { (void)data; (void)size; return false; }

/* Cheats aren't supported; the probe build logs whether the cabinet sends
   any. */
#ifdef ALP_SNAKE_PROBE
void retro_cheat_reset(void) { PROBE("retro_cheat_reset"); }
void retro_cheat_set(unsigned index, bool enabled, const char *code)
{
   struct probe_line line;
   probe_begin(&line, "retro_cheat_set index ");
   probe_number(&line, index);
   probe_add(&line, enabled ? " enabled code " : " disabled code ");
   probe_add(&line, code);
   probe_finish(&line);
}
#else
void retro_cheat_reset(void) {}
void retro_cheat_set(unsigned index, bool enabled, const char *code)
{ (void)index; (void)enabled; (void)code; }
#endif

/* libretro: the real start-up, called once Retroplayer has picked the
   "ROM" (roms/snake.zip, a placeholder we never read). Describes the
   controls the way MAME does, asks for RGB565 pixels (the load fails only
   if that is refused), seeds the game's random numbers from the clock, starts the
   synth, registers the MAME 2003-Plus variables (that is what makes the
   cabinet show its arcade menu), loads settings and high scores, enters
   attract mode, starts the backglass helper and draws the first frame. */
bool retro_load_game(const struct retro_game_info *info)
{
   enum retro_pixel_format format = RETRO_PIXEL_FORMAT_RGB565;
   (void)info;
   {
#ifdef ALP_SNAKE_PROBE
      struct probe_line line;
      probe_begin(&line, "retro_load_game path ");
      probe_add(&line, info && info->path ? info->path : "(none)");
      probe_add(&line, " size ");
      probe_number(&line, info ? info->size : 0);
      probe_finish(&line);
#endif
      /* Describe the controls and controller types the way MAME does. */
      static const struct retro_input_descriptor descriptors[] = {
         { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_LEFT, "Left" },
         { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_UP, "Up" },
         { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_DOWN, "Down" },
         { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_RIGHT, "Right" },
         { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_B, "Button 1" },
         { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_SELECT, "Coin" },
         { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_START, "Start" },
         { 0, 0, 0, 0, 0 }
      };
      bool described = environment_callback &&
         environment_callback(RETRO_ENVIRONMENT_SET_INPUT_DESCRIPTORS, (void *)descriptors);
      (void)described;
#ifdef ALP_SNAKE_PROBE
      probe_event_number("SET_INPUT_DESCRIPTORS returned", described);
#endif
      static const struct retro_controller_description pad_types[] = {
         { "Gamepad", RETRO_DEVICE_JOYPAD }
      };
      static const struct retro_controller_info controllers[] = {
         { pad_types, 1 }, { pad_types, 1 }, { 0, 0 }
      };
      bool controllers_set = environment_callback &&
         environment_callback(RETRO_ENVIRONMENT_SET_CONTROLLER_INFO, (void *)controllers);
      (void)controllers_set;
#ifdef ALP_SNAKE_PROBE
      probe_event_number("SET_CONTROLLER_INFO returned", controllers_set);
#endif
   }
   if (!environment_callback || !environment_callback(RETRO_ENVIRONMENT_SET_PIXEL_FORMAT, &format))
      return false;
   uint32_t seed = 0x534e414bu;   /* "SNAK", mixed with the clock */
   uint64_t now = monotonic_nanoseconds();
   seed ^= (uint32_t)now ^ (uint32_t)(now >> 32);
   snake_init(&game, seed);
   snake_audio_init();
#ifdef ALP_SNAKE_PROBE
   probe_event_number("SET_VARIABLES returned",
                      environment_callback(RETRO_ENVIRONMENT_SET_VARIABLES,
                                           (void *)option_variables));
   probe_check_variables();
#else
   environment_callback(RETRO_ENVIRONMENT_SET_VARIABLES, (void *)option_variables);
#endif
   load_settings();
   apply_settings();
   /* If the menu variable already reads "enabled", open the DIP page. */
   display_setup_enabled = false;
   check_display_setup();
   load_high_score();
   best_at_start = game.best;
   high_score_dirty = false;
   next_move_ns = 0;
   enter_attract();
   find_trackball();
   /* Write the state file before launching, so the helper finds it. */
   write_backglass_state();
   launch_backglass();
   draw_game();
   return true;
}

/* Special (multi-file) content isn't supported. */
bool retro_load_game_special(unsigned type, const struct retro_game_info *games, size_t count)
{
   PROBE("retro_load_game_special"); (void)type; (void)games; (void)count; return false; }

/* libretro: the player quit to the cabinet's game list. Commits a pending
   name entry (or re-saves the table if the best rose during an unfinished
   game; that game's score itself is not in the table), stops the backglass
   helper and closes the trackball. */
void retro_unload_game(void)
{
   PROBE("retro_unload_game");
   if (entering_name)
      /* Quit while typing initials: keep the score with what was typed. */
      finish_name_entry();
   else if (!attract && (game.phase == SNAKE_PLAYING || game.phase == SNAKE_PAUSED)
            && high_score_rank(game.score) >= 0)
   {
      /* Quit (Quit Game in the cabinet menu) in the middle of a game that
         already earned a place in the table: file it under the initials
         used last time rather than losing it. */
      last_score = game.score;
      start_name_entry(high_score_rank(game.score));
      finish_name_entry();
   }
   else if (high_score_dirty)
      save_high_score();
   stop_backglass();
   if (trackball_descriptor >= 0)
      system_call(SYS_close, trackball_descriptor, 0, 0, 0, 0, 0);
   trackball_descriptor = -1;
}

/* NTSC = 60 Hz. */
unsigned retro_get_region(void) { return RETRO_REGION_NTSC; }

/* No memory regions (save RAM, RTC...) are exposed: saves are our own
   files. The probe build logs the first 20 queries of each. */
void *retro_get_memory_data(unsigned id)
{
#ifdef ALP_SNAKE_PROBE
   static unsigned calls;
   if (calls++ < 20)
      probe_event_number("retro_get_memory_data id", id);
#endif
   (void)id;
   return 0;
}

/* No memory regions, so every size is 0. */
size_t retro_get_memory_size(unsigned id)
{
#ifdef ALP_SNAKE_PROBE
   static unsigned calls;
   if (calls++ < 20)
      probe_event_number("retro_get_memory_size id", id);
#endif
   (void)id;
   return 0;
}
