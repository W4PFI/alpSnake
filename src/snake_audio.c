/* snake_audio.c - ALP Snake's software synthesizer, sound effects and music.

   Small stereo synthesizer: up to 32 voices, each with an oscillator,
   envelope, optional resonant state-variable filter (low-pass, or high-pass
   for hi-hats) whose cutoff can follow the envelope ("wah"), pitch glide,
   vibrato, drive and stereo detune, all mixed through a ping-pong echo and
   a soft clipper. Freestanding: no libc or libm, so sines come from a table
   built at start-up.

   Role in the system: the front end (snake_libretro.c) decides WHEN audio is
   needed. Each retro_run it works out from the monotonic clock how many
   samples are due (not a fixed 735 per frame; capped at AUDIO_MAX_FRAMES, a
   quarter second) and calls snake_audio_render() for exactly that many. Game
   events call snake_audio_play(), which only *schedules* voices; nothing is
   computed until render runs. This "push" model keeps the sound smooth even
   when video frames arrive late.

   How the pieces fit (signal flow for one output sample):

      voice: oscillator -> filter -> drive (soft clip) -> x envelope x amp
               |  (x music fader if it is a music voice)
               +--> pan gains --> dry L/R mix
               +--> echo_send --> mono send bus
      send bus -> ping-pong delay lines -> added to L/R mix
      L/R mix -> soft clip -> x 30000 -> int16

   1. Voices. `struct voice` is a complete little monosynth. A sound effect
      is just a recipe (a play_*() function) that grabs one or more voices
      with new_voice(), then tweaks fields: glide target, filter cutoff and
      wah amount, decay half-life, vibrato, echo send. Delays let one recipe
      schedule a whole melody up front (each note gets a later start time).
      When all 32 voices are busy the oldest one is stolen.
   2. Oscillators work on a "phase" that runs 0..1 once per cycle (a phase
      accumulator): add frequency / sample_rate each sample, subtract 1 on
      wrap. Each waveform is a simple function of that phase. Left and right
      have separate phases; the right one runs `detune` times faster (e.g.
      1.005 = +8.6 cents), giving a slow beating "chorus" width.
   3. Envelope: linear attack ramp, then exponential decay (level multiplied
      by a constant every sample), then a linear release ramp to zero at the
      end of the note. Lengths are in samples.
   4. Filter: Chamberlin state-variable filter (see filter()).
   5. Echo: two 0.21 s delay lines cross-fed so repeats alternate sides.
   6. Soft clip: a rational tanh-like curve so loud moments round off
      instead of wrapping around or hard-clipping.
   7. Music: a 16-step sequencer driven by a sample counter
      (MUSIC_STEP_SAMPLES), playing a 16-bar song that loops back to bar 4.

   Numeric choices forced by having no libm (no sinf/powf/expf/tanhf/lrintf):
   - Sine: a 1024-entry table filled by repeatedly rotating a vector with
     precomputed cos/sin constants; lookup truncates, no interpolation.
   - Note pitch: a 12-entry table for one octave, doubled/halved by loops
     instead of 440 * 2^((n-69)/12).
   - Decay: 1 - ln2/(half_life*rate), the first-order approximation of
     2^(-1/(half_life*rate)).
   - Filter coefficient: 2*pi*fc/fs, the small-angle approximation of
     2*sin(pi*fc/fs), clamped to keep the filter stable.
   - Saturation: a Pade rational approximation of tanh instead of tanhf.
   - All timing is integer sample counts (unsigned), so it never drifts.
   - Per-sample divisions are avoided: 1/rate and 1/attack, 1/release are
     precomputed and multiplied instead.
   The arithmetic itself is single-precision float (aarch64 has a hardware
   FPU, and float<->int casts compile to single instructions, not library
   calls). Big buffers are `static` (zero-initialised by the loader) and
   cleared with explicit loops, because `= {0}` or struct copies of large
   objects would make the compiler emit memset/memcpy calls that do not
   exist in this -nostdlib build. */
#include "snake_audio.h"
#include "snake_game.h"

#include <stdbool.h>

/* 32 simultaneous voices: enough for the busiest music bar plus a burst of
   effects; beyond that the oldest voice is stolen. */
#define VOICES 32
/* Power of two so a phase can be wrapped into the table with a bit mask. */
#define SINE_SIZE 1024
#define ECHO_SAMPLES 9261 /* 0.21 s at 44100 Hz (9261 / 44100 = 0.21 exactly) */
#define RATE ((float)SNAKE_AUDIO_RATE)
#define INVERSE_RATE (1.0f / RATE) /* constant-folded: multiply, don't divide */

enum wave { WAVE_SINE, WAVE_TRIANGLE, WAVE_SQUARE, WAVE_SAW, WAVE_NOISE };

/* One voice = one oscillator + envelope + filter, i.e. one "note". Times are
   in samples; pitches in Hz; phases in cycles (0..1). */
struct voice {
   bool active;
   enum wave wave;
   unsigned delay;      /* samples of silence before the note starts */
   unsigned age;        /* samples played so far (0 until the delay ends) */
   unsigned length;     /* total note length; voice frees itself at the end */
   unsigned attack;     /* linear fade-in length */
   unsigned release;    /* linear fade-out length, taken from the note's end */
   float frequency;     /* start pitch */
   float glide;         /* Hz added per sample (linear pitch slide) */
   float detune;        /* right-channel pitch ratio, for stereo width */
   float vibrato_depth; /* pitch wobble as a fraction of frequency */
   float vibrato_rate;  /* wobbles per second */
   float vibrato_phase;
   float phase_left;
   float phase_right;
   float amplitude;     /* overall loudness of this voice */
   float level;         /* current exponential-decay level, starts at 1 */
   float decay;         /* per-sample multiplier applied to level */
   float cutoff;        /* filter cutoff in Hz; 0 = filter bypassed */
   float wah;           /* extra cutoff Hz at full envelope (may be negative) */
   float damping;       /* filter 1/Q: lower = more resonant "peak" */
   float low_left, band_left, low_right, band_right; /* filter state, per side */
   float gain_left;     /* equal-power pan gains */
   float gain_right;
   float echo_send;     /* how much of this voice feeds the echo */
   /* Music voices follow the music fader; a glide can stop part-way (for the
      quick pitch drop of a kick or 808); drive saturates; highpass swaps the
      filter's output for hi-hats. */
   bool music;
   unsigned glide_stop;
   float drive;
   bool highpass;
   /* 1/attack and 1/release, computed when the note actually starts (age 0)
      so recipes may change attack/release after new_voice(). */
   float inverse_attack;
   float inverse_release;
};

/* All state is static: zero-initialised for free and never copied. */
static struct voice voices[VOICES];
static float sine_table[SINE_SIZE];
static float echo_left[ECHO_SAMPLES];
static float echo_right[ECHO_SAMPLES];
static unsigned echo_position;       /* shared read/write index of both lines */
static uint32_t noise_state = 0x1234567u; /* xorshift seed; any nonzero value */
static bool ready;                   /* sine table built? */
static void music_step(void);
static bool music_playing;
static float music_gain;             /* fader applied to music voices, 0..1 */
static float music_target;           /* where the fader is heading */
static unsigned music_samples_to_step; /* countdown to the next sixteenth */

/* Builds the sine table and resets everything. The table is made by
   rotating the point (x, y) = (1, 0) by 2*pi/1024 each step:
      x' = x*c - y*s,  y' = x*s + y*c
   so y walks through sin(0), sin(2pi/1024), ... without calling sin(). The
   two constants were computed offline. Doing it in double keeps the
   accumulated rounding error after 1024 rotations far below float precision. */
void snake_audio_init(void)
{
   /* Rotate a unit vector in double precision to fill the sine table. */
   const double c = 0.99998117528260111;   /* cos(2 pi / 1024) */
   const double s = 0.0061358846491544753; /* sin(2 pi / 1024) */
   double x = 1.0;
   double y = 0.0;
   for (int index = 0; index < SINE_SIZE; ++index)
   {
      sine_table[index] = (float)y;
      double next_x = x * c - y * s;
      y = x * s + y * c;
      x = next_x;
   }
   snake_audio_stop();
   ready = true;
}

/* Hard stop: music off, all voices freed, echo tail erased (manual loops,
   since memset is unavailable). */
void snake_audio_stop(void)
{
   music_playing = false;
   music_gain = 0.0f;
   music_target = 0.0f;
   for (int index = 0; index < VOICES; ++index)
      voices[index].active = false;
   for (int index = 0; index < ECHO_SAMPLES; ++index)
   {
      echo_left[index] = 0.0f;
      echo_right[index] = 0.0f;
   }
}

/* sin(2*pi*phase) by table lookup. Phase is in cycles; the `& 1023` mask
   wraps any phase >= 1 (e.g. phase + 0.25 for cosine) back into the table.
   Phase must be non-negative: a negative float cast to unsigned is undefined. */
static float sine(float phase)
{
   return sine_table[(unsigned)(phase * SINE_SIZE) & (SINE_SIZE - 1)];
}

/* White noise from a 32-bit xorshift generator (Marsaglia's 13/17/5 shifts):
   three shifts and XORs per sample, no multiply, no libc rand(). The state is
   reinterpreted as signed and scaled by 2^-31 to give -1..1. */
static float noise(void)
{
   noise_state ^= noise_state << 13;
   noise_state ^= noise_state >> 17;
   noise_state ^= noise_state << 5;
   return (float)(int32_t)noise_state * (1.0f / 2147483648.0f);
}

/* Converts seconds to a whole number of samples (truncating). */
static unsigned seconds(float value)
{
   return (unsigned)(value * RATE);
}

/* Returns a cleared voice, stealing the oldest one if all are busy.
   delay and length are in seconds, pan is -1 (left) .. +1 (right). The
   defaults give a plain note: 4 ms attack, 30 ms release, no decay, glide,
   vibrato, filter, drive or echo; the caller then customises fields.
   Every field is assigned one by one rather than with a struct literal, so
   the compiler cannot turn it into a memcpy/memset call. A voice still in
   its start delay has age 0, so pending notes are never the ones stolen. */
static struct voice *new_voice(enum wave wave, float frequency, float delay,
                               float length, float amplitude, float pan)
{
   struct voice *voice = &voices[0];
   for (int index = 0; index < VOICES; ++index)
   {
      if (!voices[index].active)
      {
         voice = &voices[index];
         break;
      }
      if (voices[index].age > voice->age)
         voice = &voices[index];
   }
   if (pan < -1.0f)
      pan = -1.0f;
   if (pan > 1.0f)
      pan = 1.0f;
   /* Equal-power pan: angle 0..pi/2 is a quarter of the sine table, i.e.
      phase 0..0.25 cycles. Left = cos(angle) (sine a quarter cycle on),
      right = sin(angle); at centre both are 0.707, so L^2 + R^2 = 1 and a
      sound keeps the same loudness wherever it is placed. */
   float angle = (pan + 1.0f) * 0.125f;
   voice->active = true;
   voice->wave = wave;
   voice->delay = seconds(delay);
   voice->age = 0;
   voice->length = seconds(length);
   voice->attack = seconds(0.004f);
   voice->release = seconds(0.03f);
   voice->frequency = frequency;
   voice->glide = 0.0f;
   voice->detune = 1.0f;
   voice->vibrato_depth = 0.0f;
   voice->vibrato_rate = 0.0f;
   voice->vibrato_phase = 0.0f;
   voice->phase_left = 0.0f;
   voice->phase_right = 0.25f; /* quarter-cycle offset: sides differ at once */
   voice->amplitude = amplitude;
   voice->level = 1.0f;
   voice->decay = 1.0f;
   voice->cutoff = 0.0f;
   voice->wah = 0.0f;
   voice->damping = 1.0f;
   voice->low_left = voice->band_left = 0.0f;
   voice->low_right = voice->band_right = 0.0f;
   voice->gain_left = sine(angle + 0.25f);
   voice->gain_right = sine(angle);
   voice->echo_send = 0.0f;
   voice->music = false;
   voice->glide_stop = 0xffffffffu; /* "never": glide lasts the whole note */
   voice->drive = 0.0f;
   voice->highpass = false;
   return voice;
}

/* Frequency slides linearly to target over the note: glide is the number of
   Hz to add per sample, so it reaches target exactly when age == length. */
static void glide_to(struct voice *voice, float target)
{
   if (voice->length)
      voice->glide = (target - voice->frequency) / (float)voice->length;
}

/* Level falls by half every half_life seconds (first-order approximation).
   The exact per-sample factor is 2^(-1/n) with n = half_life * RATE samples;
   without powf we use 2^(-1/n) = e^(-ln2/n) ~= 1 - ln2/n, which is accurate
   to well under 0.1% for n in the hundreds or more (0.693147 = ln 2). */
static void decay(struct voice *voice, float half_life)
{
   voice->decay = 1.0f - 0.693147f / (half_life * RATE);
}

/* Maps a board column (0 .. SNAKE_COLUMNS-1) to a pan of -0.75 .. +0.75, so
   a sound comes from where it happens on the playfield without ever being
   hard in one speaker. */
static float column_pan(int column)
{
   return ((float)column - (SNAKE_COLUMNS - 1) * 0.5f) /
      ((SNAKE_COLUMNS - 1) * 0.5f) * 0.75f;
}

/* ------------------------------------------------------------------------
   Sound effect recipes. Each play_*() only configures voices; the sound is
   produced later by snake_audio_render(). Read them as data: waveform, start
   time, length, loudness, pan, then the per-voice tweaks. Note frequencies
   are equal-tempered pitches written out in Hz (e.g. 523.3 = C5, 659.3 = E5,
   784.0 = G5, 1046.5 = C6). A `delay` per note turns one call into an arpeggio.
   ------------------------------------------------------------------------ */

/* One step of the snake. variant bit 0 picks the sweep direction; the rest
   is the speed level (0-4), which raises the pitch 7% per level. */
static void play_move(float pan, unsigned variant)
{
   /* Pac-Man style "waka": alternating up and down sweeps, a little higher
      as the snake speeds up, kept quiet and dry so it never gets tiring. */
   float lift = 1.0f + 0.07f * (float)(variant >> 1);
   bool up = variant & 1;
   struct voice *voice = new_voice(WAVE_SQUARE, (up ? 250.0f : 470.0f) * lift,
                                   0.0f, 0.055f, 0.085f, pan);
   glide_to(voice, (up ? 470.0f : 250.0f) * lift);
   voice->cutoff = 900.0f;
   voice->wah = 900.0f;
   voice->damping = 1.2f;
   voice->release = seconds(0.02f);
   voice->echo_send = 0.04f;
}

/* Food eaten: quick B5-E6-B6 arpeggio (two filtered squares, then a ringing
   sine), 45 ms apart, from the snake's head position. */
static void play_eat(float pan)
{
   static const float notes[] = { 987.8f, 1318.5f, 1975.5f };
   for (int index = 0; index < 3; ++index)
   {
      struct voice *voice = new_voice(index == 2 ? WAVE_SINE : WAVE_SQUARE,
                                      notes[index], 0.045f * (float)index,
                                      index == 2 ? 0.22f : 0.08f,
                                      index == 2 ? 0.26f : 0.3f, pan);
      voice->cutoff = 2500.0f;
      voice->wah = 2500.0f;
      voice->detune = 1.004f;
      decay(voice, index == 2 ? 0.07f : 0.05f);
      voice->echo_send = 0.35f;
   }
}

/* Game start: rising C major arpeggio (C5 E5 G5 C6) that spreads across the
   stereo field, the last note held with vibrato. */
static void play_start(void)
{
   static const float notes[] = { 523.3f, 659.3f, 784.0f, 1046.5f };
   static const float pans[] = { -0.6f, 0.6f, -0.3f, 0.0f };
   for (int index = 0; index < 4; ++index)
   {
      bool last = index == 3;
      struct voice *voice = new_voice(WAVE_TRIANGLE, notes[index],
                                      0.075f * (float)index,
                                      last ? 0.45f : 0.12f, 0.3f, pans[index]);
      voice->detune = 1.005f;
      decay(voice, last ? 0.18f : 0.08f);
      voice->echo_send = 0.3f;
      if (last)
      {
         voice->vibrato_depth = 0.006f;
         voice->vibrato_rate = 6.0f;
      }
   }
}

/* Pause slides a sine down a fifth (660 -> 440 Hz); resume slides it up. */
static void play_pause(bool resume)
{
   struct voice *voice = new_voice(WAVE_SINE, resume ? 440.0f : 660.0f,
                                   0.0f, 0.14f, 0.28f, 0.0f);
   glide_to(voice, resume ? 660.0f : 440.0f);
   voice->detune = 1.006f;
   decay(voice, 0.08f);
   voice->echo_send = 0.25f;
}

/* Impact, shared by die and high score. */
static void play_crash(float pan)
{
   /* A dark noise burst where the snake hit, plus a low thump. The noise's
      filter opens to 300 + 2200 Hz at the start and closes as it decays. */
   struct voice *voice = new_voice(WAVE_NOISE, 0.0f, 0.0f, 0.35f, 0.55f, pan);
   voice->cutoff = 300.0f;
   voice->wah = 2200.0f;
   voice->damping = 1.4f;
   decay(voice, 0.05f);
   voice->echo_send = 0.2f;
   voice = new_voice(WAVE_SINE, 140.0f, 0.0f, 0.3f, 0.75f, 0.0f);
   glide_to(voice, 40.0f);
   decay(voice, 0.09f);
}

/* Game over: the crash, then a "wah wah wah waaah" trombone starting 0.45 s
   later (Eb4 D4 C#4 C4, each sagging slightly flat). */
static void play_die(float pan)
{
   play_crash(pan);
   struct voice *voice;

   /* Then a sad trombone: four sliding notes through a resonant filter that
      opens and closes like a plunger mute, detuned left and right for width. */
   static const float notes[] = { 311.1f, 293.7f, 277.2f, 261.6f };
   for (int index = 0; index < 4; ++index)
   {
      bool last = index == 3;
      float start = 0.45f + 0.5f * (float)index;
      float length = last ? 1.6f : 0.44f;
      voice = new_voice(WAVE_SAW, notes[index], start, length, 0.32f, 0.0f);
      /* Low damping (0.55) = resonant peak; wah 2000 makes the cutoff
         follow the envelope, which is the "plunger" opening and closing. */
      glide_to(voice, notes[index] * (last ? 0.97f : 0.985f));
      voice->attack = seconds(0.03f);
      voice->release = seconds(last ? 0.5f : 0.06f);
      voice->detune = 1.006f;
      voice->cutoff = 350.0f;
      voice->wah = 2000.0f;
      voice->damping = 0.55f;
      decay(voice, last ? 0.55f : 0.16f);
      voice->echo_send = 0.3f;
      if (last)
      {
         voice->vibrato_depth = 0.022f;
         voice->vibrato_rate = 5.5f;
      }
   }
}

/* Board filled: a seven-note C major fanfare bouncing left/right, ending on
   a long vibrato E6. Also the fallback for any unrecognised sound id. */
static void play_win(void)
{
   static const float notes[] = { 523.3f, 659.3f, 784.0f, 1046.5f,
                                  784.0f, 1046.5f, 1318.5f };
   for (int index = 0; index < 7; ++index)
   {
      bool last = index == 6;
      struct voice *voice = new_voice(WAVE_TRIANGLE, notes[index],
                                      0.1f * (float)index, last ? 1.0f : 0.16f,
                                      0.3f, index % 2 ? 0.5f : -0.5f);
      voice->detune = 1.005f;
      decay(voice, last ? 0.4f : 0.1f);
      voice->echo_send = 0.35f;
      if (last)
      {
         voice->vibrato_depth = 0.008f;
         voice->vibrato_rate = 6.0f;
      }
   }
}

/* Confetti cannons pop left and right, then a bright fanfare. */
static void play_high_score(float pan)
{
   play_crash(pan);
   for (int side = 0; side < 2; ++side)
   {
      struct voice *pop = new_voice(WAVE_NOISE, 0.0f, 0.3f + 0.06f * side,
                                    0.12f, 0.5f, side ? 0.9f : -0.9f);
      pop->cutoff = 1500.0f;
      pop->wah = 5000.0f;
      pop->damping = 1.0f;
      decay(pop, 0.02f);
      pop->echo_send = 0.4f;
   }
   static const float notes[] = { 523.3f, 659.3f, 784.0f, 1046.5f, 784.0f, 1046.5f };
   static const float times[] = { 0.45f, 0.55f, 0.65f, 0.75f, 0.95f, 1.05f };
   for (int index = 0; index < 6; ++index)
   {
      bool last = index == 5;
      struct voice *voice = new_voice(WAVE_SQUARE, notes[index], times[index],
                                      last ? 1.1f : 0.12f, 0.24f,
                                      index % 2 ? 0.45f : -0.45f);
      voice->cutoff = 1800.0f;
      voice->wah = 2600.0f;
      voice->detune = 1.005f;
      decay(voice, last ? 0.45f : 0.09f);
      voice->echo_send = 0.35f;
      if (last)
      {
         voice->vibrato_depth = 0.01f;
         voice->vibrato_rate = 6.0f;
         /* A fifth above the last note makes it a chord. */
         struct voice *harmony = new_voice(WAVE_TRIANGLE, 1568.0f, times[index],
                                           1.1f, 0.18f, 0.0f);
         harmony->detune = 1.006f;
         harmony->vibrato_depth = 0.01f;
         harmony->vibrato_rate = 6.0f;
         decay(harmony, 0.45f);
         harmony->echo_send = 0.35f;
      }
   }
}

/* Tiny 30 ms click for changing a letter when entering initials / settings. */
static void play_letter(void)
{
   struct voice *voice = new_voice(WAVE_SQUARE, 1200.0f, 0.0f, 0.03f, 0.12f, 0.0f);
   voice->cutoff = 2500.0f;
   decay(voice, 0.01f);
}

/* Confirmation chime (G5 C6 G6) panned left, centre, right. */
static void play_save(void)
{
   static const float notes[] = { 784.0f, 1046.5f, 1568.0f };
   for (int index = 0; index < 3; ++index)
   {
      struct voice *voice = new_voice(WAVE_TRIANGLE, notes[index],
                                      0.06f * (float)index,
                                      index == 2 ? 0.4f : 0.1f, 0.3f,
                                      (float)(index - 1) * 0.5f);
      voice->detune = 1.005f;
      decay(voice, index == 2 ? 0.15f : 0.06f);
      voice->echo_send = 0.3f;
   }
}

/* Two quick rising squeaks from the side the mouse runs in on. */
static void play_mouse(float pan)
{
   for (int index = 0; index < 2; ++index)
   {
      struct voice *voice = new_voice(WAVE_SINE, 2300.0f + 300.0f * index,
                                      0.09f * (float)index, 0.06f, 0.16f, pan);
      glide_to(voice, 3300.0f + 300.0f * index);
      voice->vibrato_depth = 0.03f;
      voice->vibrato_rate = 30.0f;
      voice->echo_send = 0.15f;
   }
}

/* A snap of the jaws, a squeak that drops away, then a bright little run. */
static void play_mouse_caught(float pan)
{
   struct voice *voice = new_voice(WAVE_NOISE, 0.0f, 0.0f, 0.06f, 0.45f, pan);
   voice->cutoff = 900.0f;
   voice->wah = 3000.0f;
   decay(voice, 0.015f);
   voice = new_voice(WAVE_SINE, 3200.0f, 0.02f, 0.12f, 0.14f, pan);
   glide_to(voice, 1400.0f);
   static const float notes[] = { 1046.5f, 1318.5f, 1568.0f, 2093.0f };
   for (int index = 0; index < 4; ++index)
   {
      voice = new_voice(WAVE_SQUARE, notes[index], 0.08f + 0.05f * (float)index,
                        index == 3 ? 0.2f : 0.07f, 0.22f, pan);
      voice->cutoff = 2500.0f;
      voice->wah = 2500.0f;
      voice->detune = 1.004f;
      decay(voice, index == 3 ? 0.08f : 0.04f);
      voice->echo_send = 0.35f;
   }
}

/* A soft two-note chime from where the sign appears. */
static void play_sign(float pan)
{
   static const float notes[] = { 1318.5f, 987.8f };
   for (int index = 0; index < 2; ++index)
   {
      struct voice *voice = new_voice(WAVE_SINE, notes[index], 0.12f * (float)index,
                                      0.35f, 0.18f, pan);
      voice->detune = 1.004f;
      decay(voice, 0.1f);
      voice->echo_send = 0.3f;
   }
}

/* "Power down": a wobbling tone that winds down like a tape slowing. */
static void play_slow_down(float pan)
{
   struct voice *voice = new_voice(WAVE_SQUARE, 900.0f, 0.0f, 0.7f, 0.26f, pan);
   glide_to(voice, 140.0f);
   voice->cutoff = 600.0f;
   voice->wah = 2200.0f;
   voice->damping = 0.8f;
   voice->detune = 1.008f;
   voice->vibrato_depth = 0.05f;
   voice->vibrato_rate = 9.0f;
   decay(voice, 0.35f);
   voice->echo_send = 0.3f;
   voice = new_voice(WAVE_TRIANGLE, 450.0f, 0.0f, 0.7f, 0.2f, -pan);
   glide_to(voice, 70.0f);
   decay(voice, 0.35f);
   voice->echo_send = 0.2f;
}

/* A hawk's "kee-eeeer": a high, raspy cry that slides down, with a fainter
   echo of it from the other side. */
static void play_screech(float pan, float delay, float level)
{
   struct voice *voice = new_voice(WAVE_SAW, 2900.0f, delay, 0.75f, level, pan);
   glide_to(voice, 1700.0f);
   voice->attack = seconds(0.04f);
   voice->release = seconds(0.25f);
   voice->cutoff = 1800.0f;
   voice->wah = 2600.0f;
   voice->damping = 0.6f;
   voice->detune = 1.012f;
   voice->vibrato_depth = 0.025f;
   voice->vibrato_rate = 28.0f;
   decay(voice, 0.45f);
   voice->echo_send = 0.45f;
   voice = new_voice(WAVE_SINE, 2600.0f, delay + 0.05f, 0.6f, level * 0.5f, -pan);
   glide_to(voice, 1500.0f);
   voice->vibrato_depth = 0.03f;
   voice->vibrato_rate = 31.0f;
   decay(voice, 0.3f);
}

/* Rushing air that rises as the hawk drops, with a cry at the start. */
static void play_hawk_dive(float pan)
{
   play_screech(pan, 0.0f, 0.2f);
   struct voice *wind = new_voice(WAVE_NOISE, 0.0f, 0.1f, 1.1f, 0.35f, pan);
   wind->cutoff = 200.0f;
   wind->wah = 0.0f;
   wind->damping = 0.7f;
   wind->attack = seconds(0.6f);
   wind->release = seconds(0.2f);
   wind->echo_send = 0.15f;
   /* Rising whistle over the wind. */
   struct voice *whistle = new_voice(WAVE_SINE, 600.0f, 0.1f, 1.1f, 0.1f, pan);
   glide_to(whistle, 1900.0f);
   whistle->attack = seconds(0.5f);
   whistle->detune = 1.01f;
}

/* Wing beats: soft thumps of filtered noise. */
static void play_wing_beats(float pan, float delay, int count)
{
   for (int beat = 0; beat < count; ++beat)
   {
      struct voice *voice = new_voice(WAVE_NOISE, 0.0f, delay + 0.14f * (float)beat,
                                      0.1f, 0.4f, pan);
      voice->cutoff = 300.0f;
      voice->wah = 900.0f;
      voice->damping = 1.2f;
      decay(voice, 0.03f);
   }
}

/* Missed: a whoosh past, wing beats climbing away, a cross cry, and a small
   G-C-E reward jingle for the dodge. */
static void play_hawk_miss(float pan)
{
   /* Negative wah: the cutoff starts low (2500 - 2000 Hz) and rises toward
      2500 Hz as the envelope decays, so the whoosh brightens as it passes. */
   struct voice *whoosh = new_voice(WAVE_NOISE, 0.0f, 0.0f, 0.35f, 0.4f, pan);
   whoosh->cutoff = 2500.0f;
   whoosh->wah = -2000.0f;
   whoosh->damping = 0.8f;
   decay(whoosh, 0.1f);
   play_wing_beats(pan, 0.15f, 4);
   play_screech(-pan, 0.4f, 0.12f);
   static const float notes[] = { 784.0f, 1046.5f, 1318.5f };
   for (int index = 0; index < 3; ++index)
   {
      struct voice *voice = new_voice(WAVE_TRIANGLE, notes[index], 0.2f + 0.07f * (float)index,
                                      index == 2 ? 0.3f : 0.1f, 0.25f, 0.0f);
      voice->detune = 1.005f;
      decay(voice, 0.1f);
      voice->echo_send = 0.3f;
   }
}

/* Caught: a triumphant cry and wing beats, then the sad trombone. */
static void play_hawk_catch(float pan)
{
   play_screech(pan, 0.0f, 0.28f);
   play_wing_beats(pan, 0.1f, 5);
   play_die(pan);
}

/* Public entry: dispatch a game event to its recipe. column positions the
   sound (see column_pan); variant is only used by MOVE. An if/else chain is
   used instead of a function-pointer table purely for readability. Note the
   final else: SNAKE_SOUND_WIN and any unknown value both play the fanfare. */
void snake_audio_play(enum snake_sound sound, int column, unsigned variant)
{
   if (!ready)
      snake_audio_init();
   float pan = column_pan(column);
   if (sound == SNAKE_SOUND_MOVE)
      play_move(pan, variant);
   else if (sound == SNAKE_SOUND_EAT)
      play_eat(pan);
   else if (sound == SNAKE_SOUND_START)
   {
      /* Cut any lingering effects, but let the music fade on its own. */
      for (int index = 0; index < VOICES; ++index)
         if (!voices[index].music)
            voices[index].active = false;
      play_start();
   }
   else if (sound == SNAKE_SOUND_PAUSE || sound == SNAKE_SOUND_RESUME)
      play_pause(sound == SNAKE_SOUND_RESUME);
   else if (sound == SNAKE_SOUND_DIE)
      play_die(pan);
   else if (sound == SNAKE_SOUND_HIGH_SCORE)
      play_high_score(pan);
   else if (sound == SNAKE_SOUND_LETTER)
      play_letter();
   else if (sound == SNAKE_SOUND_SAVE)
      play_save();
   else if (sound == SNAKE_SOUND_MOUSE)
      play_mouse(pan);
   else if (sound == SNAKE_SOUND_MOUSE_CAUGHT)
      play_mouse_caught(pan);
   else if (sound == SNAKE_SOUND_SIGN)
      play_sign(pan);
   else if (sound == SNAKE_SOUND_SLOW_DOWN)
      play_slow_down(pan);
   else if (sound == SNAKE_SOUND_HAWK_SCREECH)
      play_screech(pan, 0.0f, 0.22f);
   else if (sound == SNAKE_SOUND_HAWK_DIVE)
      play_hawk_dive(pan);
   else if (sound == SNAKE_SOUND_HAWK_MISS)
      play_hawk_miss(pan);
   else if (sound == SNAKE_SOUND_HAWK_CATCH)
      play_hawk_catch(pan);
   else
      play_win();
}

/* One sample of a waveform at `phase` (0..1 cycles). These are "naive"
   (non-band-limited) shapes: cheap, and the aliasing they add at high notes
   is mostly tamed by the per-voice low-pass filter.
   - triangle: up from -1 to +1 over the first half, back down over the second
   - square:   +-0.7 rather than +-1, roughly matching the other waves' loudness
   - saw:      falls from +1 to -1 over the cycle
   - noise:    ignores phase entirely */
static float oscillator(enum wave wave, float phase)
{
   if (wave == WAVE_SINE)
      return sine(phase);
   if (wave == WAVE_TRIANGLE)
      return phase < 0.5f ? 4.0f * phase - 1.0f : 3.0f - 4.0f * phase;
   if (wave == WAVE_SQUARE)
      return phase < 0.5f ? 0.7f : -0.7f;
   if (wave == WAVE_SAW)
      return 1.0f - 2.0f * phase;
   return noise();
}

/* Chamberlin state-variable filter: returns the low-pass output, or the
   high-pass output when `highpass` is set (used for hi-hats).
   Two integrators in a loop, run once per sample:
      high = input - low - damping * band   (what the integrators miss)
      band = band + f * high                (first integrator: band-pass)
      low  = low  + f * band                (second integrator: low-pass)
   f = coefficient = 2*sin(pi * cutoff / rate); the caller passes the
   small-angle approximation 2*pi*cutoff/rate (no sinf), which is close at
   low cutoffs and makes high cutoffs come out somewhat lower than asked.
   damping is 1/Q: 2 is very soft, below ~0.7 a resonant peak appears at
   the cutoff (the vocal "wah"/nasal sound). Working through the update as a
   2x2 matrix, the filter stays stable while f*damping < 2 and
   f^2 + 2*f*damping < 4; with f clamped to 0.9 by the caller that holds for
   every damping used here (max 1.4). *low and *band are the voice's state,
   kept separately for the left and right channels. */
static float filter(float input, float coefficient, float damping,
                    float *low, float *band, bool highpass)
{
   float high = input - *low - damping * *band;
   *band += coefficient * high;
   *low += coefficient * *band;
   return highpass ? high : *low;
}


/* Smooth saturation, a stand-in for tanhf(): the rational (Pade-style)
   approximation x * (27 + x^2) / (27 + 9 x^2). It is ~x for small inputs
   (so quiet audio passes unchanged), bends gently as the input grows, and
   equals exactly +-1 at x = +-3, where the code switches to a flat +-1, so
   the curve is continuous. Used for per-voice drive (808, kick: adds
   harmonics that small cabinet speakers can reproduce) and on the final mix
   so overloads round off instead of wrapping around in int16. */
static float soft_clip(float value)
{
   if (value > 3.0f)
      return 1.0f;
   if (value < -3.0f)
      return -1.0f;
   float square = value * value;
   return value * (27.0f + square) / (27.0f + 9.0f * square);
}

/* ------------------------------------------------------------------------
   Attract music: an original "Egyptian trap" piece in E Hijaz (E F G# A B C
   D), 140 BPM in half time. A nasal mizmar-style lead and oud-like plucks
   over darbuka, claps, rolling hi-hats and a saturated 808.

   Sequencer: the song is a grid of 16 bars x 16 sixteenth-note steps. The
   music clock is a sample countdown inside snake_audio_render(): every
   MUSIC_STEP_SAMPLES output samples it calls music_step(), which starts
   whatever voices fall on the current step and advances the position.
   Because time is counted in samples, not video frames, tempo is exact
   no matter how retro_run calls are spaced; and notes start on an exact
   sample, independent of how big each render buffer is.

   Song form (bar numbers from 0): bars 0-3 intro (melody A, plucks; hats
   from bar 2, bass from bar 2), bars 4-7 "the drop" (melody A, full drums),
   bars 8-11 melody B, a step higher in feel, bars 12-15 melody A again. The
   last bar of each 4-bar phrase has a hi-hat roll. After bar 15 it loops to
   bar 4, so the intro is only heard once (~27 s first pass, ~20.6 s loop).
   ------------------------------------------------------------------------ */
/* One sixteenth note at 140 BPM: a beat is 60/140 s = 18900 samples at
   44100 Hz, and a sixteenth is a quarter of that, 4725 samples (~107 ms). A
   16-step bar is 75600 samples, about 1.71 s. */
#define MUSIC_STEP_SAMPLES 4725 /* one sixteenth note at 140 BPM */
#define SONG_BARS 16
#define SONG_LOOP_BAR 4         /* after the intro, loop from the drop */

/* One melody note within a bar: start step (0-15), pitch, duration. */
struct note_event {
   uint8_t step;
   uint8_t note;   /* MIDI note number */
   uint8_t length; /* in sixteenths */
};
/* Sentinel: step 255 can never match a real step, so it ends the list. */
#define END_OF_BAR { 255, 0, 0 }

/* Lead phrases, one bar each. E5 = 76. In E Hijaz the notes used are
   64 E4, 65 F4, 68 G#4, 69 A4, 71 B4, 72 C5, 74 D5, 76 E5, 77 F5, 80 G#5,
   81 A5; the F-to-G# augmented second gives the "Middle Eastern" flavour. */
static const struct note_event lead_a1[] = {
   {0, 76, 3}, {3, 77, 1}, {4, 80, 2}, {6, 77, 2}, {8, 76, 4}, {12, 74, 2},
   {14, 76, 2}, END_OF_BAR };
static const struct note_event lead_a2[] = {
   {0, 77, 2}, {2, 76, 2}, {4, 74, 2}, {6, 72, 2}, {8, 71, 6}, END_OF_BAR };
static const struct note_event lead_a3[] = {
   {0, 71, 2}, {2, 72, 2}, {4, 74, 2}, {6, 72, 1}, {7, 71, 1}, {8, 69, 4},
   {12, 68, 2}, {14, 69, 2}, END_OF_BAR };
static const struct note_event lead_a4[] = {
   {0, 71, 3}, {3, 72, 1}, {4, 71, 2}, {6, 69, 2}, {8, 68, 2}, {10, 65, 2},
   {12, 64, 4}, END_OF_BAR };
static const struct note_event lead_b1[] = {
   {0, 76, 1}, {1, 77, 1}, {2, 76, 1}, {3, 77, 1}, {4, 80, 4}, {8, 81, 2},
   {10, 80, 2}, {12, 77, 4}, END_OF_BAR };
static const struct note_event lead_b2[] = {
   {0, 76, 2}, {2, 77, 2}, {4, 76, 2}, {6, 74, 2}, {8, 76, 8}, END_OF_BAR };
static const struct note_event lead_b3[] = {
   {0, 72, 2}, {2, 74, 2}, {4, 76, 4}, {8, 77, 2}, {10, 76, 2}, {12, 74, 2},
   {14, 72, 2}, END_OF_BAR };
static const struct note_event lead_b4[] = {
   {0, 71, 4}, {4, 72, 2}, {6, 71, 2}, {8, 68, 2}, {10, 65, 2}, {12, 64, 4},
   END_OF_BAR };

/* Chords for the plucks, by bass root: E (E G# B), A (A C E), C (C E A). */
static const uint8_t chord_e[3] = { 64, 68, 71 };
static const uint8_t chord_a[3] = { 69, 72, 76 };
static const uint8_t chord_c[3] = { 60, 64, 69 };

/* Drum intensity per bar. Ordered so `>=` tests work: ROLL includes FULL,
   FULL includes HATS. */
enum { DRUMS_NONE, DRUMS_HATS, DRUMS_FULL, DRUMS_ROLL };

/* One row of the arrangement. Everything other than the lead melody is a
   fixed pattern in music_step(), parameterised by these fields. */
struct song_bar {
   const struct note_event *lead;
   uint8_t bass_root;   /* MIDI, 0 = no bass (40 = E2, 45 = A2, 36 = C2) */
   const uint8_t *chord;
   uint8_t drums;
};

static const struct song_bar song[SONG_BARS] = {
   /* Intro: melody and plucks, hats then bass creep in. */
   { lead_a1, 0, chord_e, DRUMS_NONE },
   { lead_a2, 0, chord_e, DRUMS_NONE },
   { lead_a3, 45, chord_a, DRUMS_HATS },
   { lead_a4, 40, chord_e, DRUMS_HATS },
   /* The drop. */
   { lead_a1, 40, chord_e, DRUMS_FULL },
   { lead_a2, 40, chord_e, DRUMS_FULL },
   { lead_a3, 45, chord_a, DRUMS_FULL },
   { lead_a4, 40, chord_e, DRUMS_ROLL },
   /* Second section, higher. */
   { lead_b1, 40, chord_e, DRUMS_FULL },
   { lead_b2, 40, chord_e, DRUMS_FULL },
   { lead_b3, 36, chord_c, DRUMS_FULL },
   { lead_b4, 40, chord_e, DRUMS_ROLL },
   /* First melody again over the full beat. */
   { lead_a1, 40, chord_e, DRUMS_FULL },
   { lead_a2, 40, chord_e, DRUMS_FULL },
   { lead_a3, 45, chord_a, DRUMS_FULL },
   { lead_a4, 40, chord_e, DRUMS_ROLL },
};

/* Sequencer position: current bar (0-15) and step within it (0-15). */
static unsigned music_bar;
static unsigned music_sixteenth;

/* Equal-tempered frequency of a MIDI note, without libm. The table holds
   one octave, MIDI 60-71 (C4 = 261.626 Hz ... B4); note / 12 - 5 is how many
   octaves away the note is, applied by exact doubling or halving instead of
   computing 440 * 2^((note - 69) / 12) with powf. */
static float note_frequency(unsigned note)
{
   static const float octave[12] = {
      261.626f, 277.183f, 293.665f, 311.127f, 329.628f, 349.228f,
      369.994f, 391.995f, 415.305f, 440.000f, 466.164f, 493.883f
   };
   float frequency = octave[note % 12];
   int shift = (int)(note / 12) - 5;
   while (shift > 0)
   {
      frequency *= 2.0f;
      --shift;
   }
   while (shift < 0)
   {
      frequency *= 0.5f;
      ++shift;
   }
   return frequency;
}

/* Converts a duration in sixteenth notes to seconds (for new_voice). */
static float step_seconds(float sixteenths)
{
   return sixteenths * (float)MUSIC_STEP_SAMPLES / RATE;
}

/* new_voice() for the song: tags the voice so it follows the music fader
   and is cut when the music stops (sound effects are left alone). */
static struct voice *music_voice(enum wave wave, float frequency, float delay,
                                 float length, float amplitude, float pan)
{
   struct voice *voice = new_voice(wave, frequency, delay, length, amplitude, pan);
   voice->music = true;
   return voice;
}

/* Mizmar-like lead: a saw through a resonant low-pass, plus a quiet square
   an octave below. Notes last 95% of their written length so repeated
   notes are separated by a small gap (articulation). */
static void play_lead(unsigned note, unsigned length)
{
   float frequency = note_frequency(note);
   struct voice *voice = music_voice(WAVE_SAW, frequency, 0.0f,
                                     step_seconds((float)length) * 0.95f, 0.15f, 0.1f);
   voice->attack = seconds(0.012f);
   voice->release = seconds(0.04f);
   voice->cutoff = 1300.0f;
   voice->wah = 1100.0f;
   voice->damping = 0.45f;          /* resonant: the reedy, nasal tone */
   voice->detune = 1.004f;
   voice->vibrato_depth = 0.012f;
   voice->vibrato_rate = 6.0f;
   decay(voice, 0.9f);
   voice->echo_send = 0.22f;
   /* A quiet octave-down double for body. */
   voice = music_voice(WAVE_SQUARE, frequency * 0.5f, 0.0f,
                       step_seconds((float)length) * 0.95f, 0.05f, -0.2f);
   voice->cutoff = 900.0f;
   voice->damping = 1.0f;
   decay(voice, 0.9f);
}

/* Oud-like pluck: near-instant attack, fast decay, filter that snaps open
   with the envelope (700 + 2600 Hz) and closes as the note dies. */
static void play_pluck(unsigned note, float pan)
{
   struct voice *voice = music_voice(WAVE_SQUARE, note_frequency(note), 0.0f,
                                     0.3f, 0.11f, pan);
   voice->attack = seconds(0.002f);
   voice->cutoff = 700.0f;
   voice->wah = 2600.0f;
   voice->damping = 0.9f;
   decay(voice, 0.07f);
   voice->echo_send = 0.3f;
}

/* Trap "808" bass: a sine that starts 2.2x above the note, drops to it in
   30 ms (glide_stop ends the glide there), then holds, saturated by drive. */
static void play_808(unsigned note, unsigned length)
{
   float frequency = note_frequency(note);
   struct voice *voice = music_voice(WAVE_SINE, frequency * 2.2f, 0.0f,
                                     step_seconds((float)length), 0.42f, 0.0f);
   /* Fast drop from the attack pitch to the note, then hold. */
   voice->glide_stop = seconds(0.03f);
   voice->glide = (frequency - frequency * 2.2f) / (float)voice->glide_stop;
   voice->attack = seconds(0.003f);
   voice->release = seconds(0.05f);
   voice->drive = 2.2f;             /* harmonics so small speakers carry it */
   decay(voice, 0.6f);
}

/* Kick drum: a 150 -> 45 Hz sine pitch drop in 70 ms (the body) plus an
   8 ms filtered noise tick (the beater click). */
static void play_kick(void)
{
   struct voice *voice = music_voice(WAVE_SINE, 150.0f, 0.0f, 0.25f, 0.62f, 0.0f);
   voice->glide_stop = seconds(0.07f);
   voice->glide = (45.0f - 150.0f) / (float)voice->glide_stop;
   voice->attack = seconds(0.001f);
   voice->drive = 1.5f;
   decay(voice, 0.07f);
   voice = music_voice(WAVE_NOISE, 0.0f, 0.0f, 0.008f, 0.25f, 0.0f);
   voice->cutoff = 3000.0f;
}

/* Trap-style hand clap for the music: filtered noise bursts, slightly
   staggered in time and spread across the stereo field. */
static void play_clap(void)
{
   /* Three quick bursts, like several hands, then a short tail. */
   for (int hit = 0; hit < 3; ++hit)
   {
      struct voice *voice = music_voice(WAVE_NOISE, 0.0f, 0.009f * (float)hit,
                                        hit == 2 ? 0.2f : 0.012f, 0.32f,
                                        (float)(hit - 1) * 0.25f);
      voice->cutoff = 1200.0f;
      voice->wah = 1800.0f;
      voice->damping = 0.6f;
      decay(voice, hit == 2 ? 0.05f : 0.01f);
      voice->echo_send = 0.3f;
   }
}

/* Hi-hat: very short high-passed noise. Note 7000 Hz gives a coefficient
   of ~1.0, which render clamps to 0.9 (an effective corner near 6.3 kHz). */
static void play_hat(float delay, float level, float pan)
{
   struct voice *voice = music_voice(WAVE_NOISE, 0.0f, delay, 0.05f, level, pan);
   voice->cutoff = 7000.0f;
   voice->highpass = true;
   voice->damping = 1.2f;
   decay(voice, 0.012f);
}

/* Darbuka: the deep "doum" and the sharp "tek". Doum: a 210 -> 120 Hz
   sine drop plus a dull noise slap. */
static void play_doum(void)
{
   struct voice *voice = music_voice(WAVE_SINE, 210.0f, 0.0f, 0.22f, 0.3f, -0.15f);
   voice->glide_stop = seconds(0.05f);
   voice->glide = (120.0f - 210.0f) / (float)voice->glide_stop;
   decay(voice, 0.08f);
   voice = music_voice(WAVE_NOISE, 0.0f, 0.0f, 0.03f, 0.12f, -0.15f);
   voice->cutoff = 600.0f;
}

/* Tek: a bright, resonant noise crack with a faint 820 Hz ring. */
static void play_tek(float pan)
{
   struct voice *voice = music_voice(WAVE_NOISE, 0.0f, 0.0f, 0.05f, 0.2f, pan);
   voice->cutoff = 2500.0f;
   voice->wah = 3000.0f;
   voice->damping = 0.5f;
   decay(voice, 0.015f);
   voice = music_voice(WAVE_SINE, 820.0f, 0.0f, 0.03f, 0.08f, pan);
   decay(voice, 0.01f);
}

/* Called once per sixteenth note: start whatever falls on this step, then
   advance the position (wrapping the step at 16, and the bar from 15 back to
   SONG_LOOP_BAR). Only the lead melody comes from data; bass, plucks and
   drums are fixed patterns switched on by the bar's fields. Steps 0, 4, 8, 12
   are the four beats; with the clap on step 8 the groove feels "half time"
   (70 BPM) although the grid runs at 140. */
static void music_step(void)
{
   const struct song_bar *bar = &song[music_bar];
   unsigned step = music_sixteenth;

   for (const struct note_event *note = bar->lead; note->step != 255; ++note)
      if (note->step == step)
         play_lead(note->note, note->length);

   /* Oud-like plucks on the off-beats, walking the chord: steps 2,3,6 play
      chord notes 0,1,2 and steps 10,11,14 repeat them, alternating sides. */
   static const uint8_t pluck_steps[6] = { 2, 3, 6, 10, 11, 14 };
   for (int index = 0; index < 6; ++index)
      if (pluck_steps[index] == step)
         play_pluck(bar->chord[index % 3], index % 2 ? 0.45f : -0.45f);

   /* 808 rhythm (same steps as the kick below): long note on the downbeat,
      syncopated hits on 7 and 10, an octave jump on 14. */
   if (bar->bass_root)
   {
      if (step == 0)
         play_808(bar->bass_root, 6);
      else if (step == 7)
         play_808(bar->bass_root, 2);
      else if (step == 10)
         play_808(bar->bass_root, 3);
      else if (step == 14)
         play_808(bar->bass_root + 12, 2);
   }

   /* Eighth-note hats, accented on the beats (step % 4 == 0). */
   if (bar->drums >= DRUMS_HATS && step % 2 == 0)
      play_hat(0.0f, step % 4 ? 0.07f : 0.11f, step % 4 ? 0.3f : -0.3f);
   if (bar->drums >= DRUMS_FULL)
   {
      if (step == 0 || step == 7 || step == 10)
         play_kick();
      if (step == 8)
         play_clap();
      /* Maqsum-style darbuka under the beat. */
      if (step == 0 || step == 9)
         play_doum();
      if (step == 3 || step == 6 || step == 12 || step == 14)
         play_tek(step % 2 ? 0.4f : -0.4f);
      /* Trap hat flourishes: extra off-grid sixteenths here; the 32nd-note
         roll is added below in DRUMS_ROLL bars. */
      if (step == 5 || step == 13)
         play_hat(0.0f, 0.06f, 0.2f);
   }
   /* Roll over the last beat (steps 12-15): a hat on the step plus one half
      a step later (32nds); on 14-15 another a quarter step in, so the roll
      speeds up into the next bar. Delays schedule the sub-step hits. */
   if (bar->drums == DRUMS_ROLL && step >= 12)
   {
      play_hat(0.0f, 0.08f, 0.35f);
      play_hat(step_seconds(0.5f), 0.07f, -0.35f);
      if (step >= 14)
         play_hat(step_seconds(0.25f), 0.06f, 0.0f);
   }

   if (++music_sixteenth == 16)
   {
      music_sixteenth = 0;
      if (++music_bar == SONG_BARS)
         music_bar = SONG_LOOP_BAR;
   }
}

/* Starts (from bar 0, with a fade-in) or fades out the attract music. The
   counter is set to 0 so the first step fires on the very next sample.
   Turning it on while it is still fading out just reverses the fade. */
void snake_audio_music(int on)
{
   if (!ready)
      snake_audio_init();
   if (on)
   {
      if (!music_playing)
      {
         music_bar = 0;
         music_sixteenth = 0;
         music_samples_to_step = 0;
         music_gain = 0.0f;
         music_playing = true;
      }
      music_target = 1.0f;
   }
   else
      music_target = 0.0f;
}

/* True while music is audible and not on its way out. */
int snake_audio_music_playing(void)
{
   return music_playing && music_target > 0.0f;
}

/* The audio engine: produce `frames` stereo samples. For each output frame:
   1. advance the music clock and fader,
   2. run every active voice (delay, envelope, pitch, oscillator, filter,
      drive) and add it to the dry left/right mix and the echo send,
   3. run the ping-pong echo,
   4. soft-clip and convert to int16.
   The front end decides `frames` from elapsed real time, so this function
   has no notion of video frames at all. */
void snake_audio_render(int16_t *stereo, unsigned frames)
{
   if (!ready)
      snake_audio_init();
   for (unsigned frame = 0; frame < frames; ++frame)
   {
      float left = 0.0f;
      float right = 0.0f;
      float send = 0.0f;
      if (music_playing)
      {
         /* Sample-accurate sequencer clock: when the countdown is 0, play a
            step and reload with STEP-1 (this sample counts as the first),
            so steps are exactly MUSIC_STEP_SAMPLES apart. The new voices
            start sounding in this same sample, in the loop below. */
         if (!music_samples_to_step--)
         {
            music_step();
            music_samples_to_step = MUSIC_STEP_SAMPLES - 1;
         }
         /* Fades take about half a second: 1 / 0.00005 = 20000 samples
            (0.45 s). A linear ramp per sample, so no zipper steps. When a
            fade-out reaches 0 the music voices are cut and the sequencer
            stops. */
         if (music_gain < music_target)
            music_gain = music_gain + 0.00005f > music_target ?
               music_target : music_gain + 0.00005f;
         else if (music_gain > music_target)
         {
            music_gain -= 0.00005f;
            if (music_gain <= 0.0f)
            {
               music_gain = 0.0f;
               music_playing = false;
               for (int index = 0; index < VOICES; ++index)
                  if (voices[index].music)
                     voices[index].active = false;
            }
         }
      }
      for (int index = 0; index < VOICES; ++index)
      {
         struct voice *voice = &voices[index];
         if (!voice->active)
            continue;
         /* Scheduled but not started yet: count down silently. */
         if (voice->delay)
         {
            --voice->delay;
            continue;
         }
         if (voice->age >= voice->length)
         {
            voice->active = false;
            continue;
         }
         /* First audible sample: precompute reciprocals so the envelope
            below multiplies instead of dividing every sample. */
         if (voice->age == 0)
         {
            voice->inverse_attack = voice->attack ? 1.0f / (float)voice->attack : 0.0f;
            voice->inverse_release = voice->release ? 1.0f / (float)voice->release : 0.0f;
         }
         /* Envelope = exponential decay level x attack ramp (0 -> 1 over
            `attack` samples) x release ramp (1 -> 0 over the last `release`
            samples). The ramps can overlap on very short notes. */
         float envelope = voice->level;
         if (voice->age < voice->attack)
            envelope *= (float)voice->age * voice->inverse_attack;
         unsigned remaining = voice->length - voice->age;
         if (remaining < voice->release)
            envelope *= (float)remaining * voice->inverse_release;
         voice->level *= voice->decay;

         /* Pitch = start + glide * elapsed samples, frozen once age reaches
            glide_stop (computed from age, not accumulated, so no drift). */
         float frequency = voice->frequency + voice->glide *
            (float)(voice->age < voice->glide_stop ? voice->age : voice->glide_stop);
         if (voice->vibrato_depth > 0.0f)
         {
            /* Vibrato fades in over the first half second (22050 samples at
               44.1 kHz), then multiplies the pitch by 1 + depth*sin(LFO).
               Consequence: on short notes (e.g. the 60 ms mouse squeak)
               only a fraction of the set depth is ever reached. */
            float depth = voice->vibrato_depth *
               (voice->age < 22050 ? (float)voice->age * (1.0f / 22050.0f) : 1.0f);
            frequency *= 1.0f + depth * sine(voice->vibrato_phase);
            voice->vibrato_phase += voice->vibrato_rate * INVERSE_RATE;
            if (voice->vibrato_phase >= 1.0f)
               voice->vibrato_phase -= 1.0f;
         }
         /* Noise is drawn once and used for both sides (calling noise()
            twice would give decorrelated, very wide noise). Other waves
            read their own left/right phases; the right advances `detune`
            times faster, producing the slow stereo beating. */
         float sample_left = oscillator(voice->wave, voice->phase_left);
         float sample_right = voice->wave == WAVE_NOISE ? sample_left :
            oscillator(voice->wave, voice->phase_right);
         voice->phase_left += frequency * INVERSE_RATE;
         voice->phase_right += frequency * voice->detune * INVERSE_RATE;
         if (voice->phase_left >= 1.0f)
            voice->phase_left -= 1.0f;
         if (voice->phase_right >= 1.0f)
            voice->phase_right -= 1.0f;

         /* Filter. The cutoff tracks the envelope by `wah` Hz, so notes are
            brightest at their loudest and darken as they fade. The
            coefficient is clamped at 0.9 to keep the filter stable. */
         if (voice->cutoff > 0.0f)
         {
            float cutoff = voice->cutoff + voice->wah * envelope;
            float coefficient = 6.2831853f * INVERSE_RATE * cutoff;
            if (coefficient > 0.9f)
               coefficient = 0.9f;
            sample_left = filter(sample_left, coefficient, voice->damping,
                                 &voice->low_left, &voice->band_left,
                                 voice->highpass);
            sample_right = filter(sample_right, coefficient, voice->damping,
                                  &voice->low_right, &voice->band_right,
                                  voice->highpass);
         }
         if (voice->drive > 0.0f)
         {
            sample_left = soft_clip(sample_left * voice->drive);
            sample_right = soft_clip(sample_right * voice->drive);
         }
         /* Mix: dry signal to each side through the pan gains, and the mono
            average into the echo send bus. */
         float amount = envelope * voice->amplitude;
         if (voice->music)
            amount *= music_gain;
         left += sample_left * amount * voice->gain_left;
         right += sample_right * amount * voice->gain_right;
         send += (sample_left + sample_right) * 0.5f * amount * voice->echo_send;
         ++voice->age;
      }

      /* Ping-pong echo: each repeat bounces to the other speaker. Two
         circular buffers of ECHO_SAMPLES share one index: read the sample
         written 0.21 s ago, then overwrite it. The send enters only the
         left line; whatever comes out of the left is fed (x 0.42) into the
         right line and vice versa. So the first repeat is heard on the
         left after 0.21 s, the next on the right at 0.42 s, each 0.42x
         (about -7.5 dB) quieter, dying away within a couple of seconds.
         Repeats are mixed in at 0.7. */
      float delayed_left = echo_left[echo_position];
      float delayed_right = echo_right[echo_position];
      echo_left[echo_position] = send + delayed_right * 0.42f;
      echo_right[echo_position] = delayed_left * 0.42f;
      if (++echo_position == ECHO_SAMPLES)
         echo_position = 0;
      left += delayed_left * 0.7f;
      right += delayed_right * 0.7f;

      /* Final limiter + conversion. soft_clip keeps |x| <= 1, and 30000
         (not 32767) leaves a little headroom; the cast truncates, which is
         fine since there is no lrintf and the error is below 1 LSB. */
      stereo[frame * 2] = (int16_t)(soft_clip(left) * 30000.0f);
      stereo[frame * 2 + 1] = (int16_t)(soft_clip(right) * 30000.0f);
   }
}
