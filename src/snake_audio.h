/* snake_audio.h - public interface of ALP Snake's software synthesizer.

   Role in the system: the libretro front end (snake_libretro.c) owns the
   audio *timing*; this module owns the audio *content*. The front end calls
   snake_audio_play() when a game event happens (food eaten, crash, hawk...),
   snake_audio_music() to start/stop the attract-mode tune, and once per
   retro_run snake_audio_render() to fill a buffer with however many samples
   are due by the real clock. It then pushes that buffer to Retroplayer.

   Key ideas for a learner:
   - Nothing here touches hardware or libretro. The synth is a pure function
     of "events in" and "samples out", so it could be reused in any program.
   - Sounds are not recorded files; every effect and the whole song are
     generated from a few numbers at run time (see snake_audio.c).
   - The output format is 16-bit signed, interleaved stereo (L, R, L, R...)
     at SNAKE_AUDIO_RATE samples per second per channel. */
#ifndef ALP_SNAKE_AUDIO_H
#define ALP_SNAKE_AUDIO_H

#include <stdint.h>

/* Output sample rate in Hz. All synth timing (note lengths, delays, the
   sequencer's step length) is counted in samples at this rate. */
#define SNAKE_AUDIO_RATE 44100

/* Every sound effect the game can request. Each maps to one play_*()
   recipe in snake_audio.c; a recipe may start several voices at once. */
enum snake_sound {
   SNAKE_SOUND_MOVE,
   SNAKE_SOUND_EAT,
   SNAKE_SOUND_START,
   SNAKE_SOUND_PAUSE,
   SNAKE_SOUND_RESUME,
   SNAKE_SOUND_DIE,
   SNAKE_SOUND_WIN,
   SNAKE_SOUND_HIGH_SCORE,
   SNAKE_SOUND_LETTER,
   SNAKE_SOUND_SAVE,
   SNAKE_SOUND_MOUSE,
   SNAKE_SOUND_MOUSE_CAUGHT,
   SNAKE_SOUND_SIGN,
   SNAKE_SOUND_SLOW_DOWN,
   SNAKE_SOUND_HAWK_SCREECH,
   SNAKE_SOUND_HAWK_DIVE,
   SNAKE_SOUND_HAWK_MISS,
   SNAKE_SOUND_HAWK_CATCH
};

/* Builds the sine table and silences everything. Safe to call again; the
   other entry points call it lazily if the front end has not. */
void snake_audio_init(void);
/* Silences all voices, the music and the echo tail immediately. */
void snake_audio_stop(void);
/* column places the sound in the stereo field (0 = far left of the board).
   variant: for MOVE, bit 0 alternates the "waka" direction and the remaining
   bits give the speed level. */
void snake_audio_play(enum snake_sound sound, int column, unsigned variant);
/* Synthesizes `frames` stereo frames into `stereo`, which must hold
   2 * frames int16_t values (left then right for each frame). */
void snake_audio_render(int16_t *stereo, unsigned frames);
/* Attract-mode music: nonzero starts it from the top (if not already
   playing; if it is still fading out it fades back in where it is); zero
   fades it out. */
void snake_audio_music(int on);
/* Nonzero while music is playing and not fading out. */
int snake_audio_music_playing(void);

#endif
