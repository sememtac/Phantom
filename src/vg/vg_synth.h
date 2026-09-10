#pragma once
#include <stdint.h>

// ===========================================================================
// THE SYNTH
//
// How sound is MADE. What the game's sounds ARE lives in vg_sfx.cpp, which is a
// table rather than code -- the split exists so that adding a cue is adding
// data, and changing how a voice behaves cannot quietly change what a cue is.
//
// A voice is an oscillator, an envelope with a hold in it, a pitch sweep and a
// two-pole low pass. That covers everything this game asks for: a beep is a
// square that holds, a click is noise that stops before it is a sound, an
// explosion is the same generator with an order of magnitude more life and far
// more taken off the top.
//
// NOTHING HERE TOUCHES THE SIMULATION. It draws no random numbers from the
// game's seeded stream, sets no state the game reads, and takes no decision.
// Replay determinism depends on the sim being a pure function of (seed, dt,
// input), and a sound that could nudge any of those would take the capture tool
// down with it.
// ===========================================================================

enum SynthWave : unsigned char { SW_SQUARE = 0, SW_NOISE, SW_SINE };

// One layer of a cue. Cues are built from these, in tables.
struct SynthLayer {
    SynthWave wave;
    float f0, f1;       // Hz, swept f0 -> f1 across the life. Ignored for noise.
    float life;         // seconds
    float attack;       // seconds to full
    // Fraction of the life HELD at full before the decay starts. Zero is a plain
    // rise-and-fall, which is right for a click and wrong for anything meant to
    // read as sustained: a decaying beat is an event, something that holds and
    // then gives way is a thing failing.
    float sustain;
    float gain;
    float lp_hz;        // two-pole low pass corner
    float delay;        // seconds before it starts, for sequences and two-tones
    // Amplitude modulation. What turns a plain square into something reedy -- the
    // difference between a tone and a quack, or a thump and a growl, is almost
    // entirely this. Under about 15 Hz it is heard as juddering; above about 30
    // it fuses into timbre.
    float mod_hz, mod_depth;
};

// Voices currently sounding in both pools, for the profiler.
int vg_synth_live(void);

void vg_synth_reset(void);

// Stop everything now, held sounds included, with no ramp and no tail.
void vg_synth_silence(void);

// Start one layer of a CUE. `pitch` scales its frequencies; 1.0 is as written.
// Takes a voice from the cue pool, and only from there.
void vg_synth_layer(const SynthLayer* l, float pitch);

// Start one NOTE of the music. `gain` scales its level; 1.0 is as written, and
// the score player passes the music setting through it.
//
// TWO POOLS, AND NEITHER CAN TOUCH THE OTHER. The music used to share the ten cue
// voices with no priority, and it lost twice over: a hull hit is seven layers and
// took the notes that were sounding, and the score player stopped adding notes
// whenever the pool was near full, which in a fight is most of the time. Either
// way the composition had holes in it. A note now comes from the music's own
// eight voices, sized to the thickest bar any score has (seven, in the final's
// theme), so an alert can never take a note and a note can never take an alert.
// When a score does ask for more than eight, the note with least left to play
// gives way -- the same rule the cues use, applied within the music alone.
void vg_synth_note(const SynthLayer* l, float gain);

// The continuous drive. Not a layer and not in the voice pool: it is on for as
// long as the ship is flying, and a one-shot retriggered forever would either
// gap or overlap itself.
void vg_synth_engine(bool on, float throttle);

// The flatline. Held like the engine and for the same reason -- it runs for as
// long as the game says somebody is dead, which on the wreck screen is until the
// player decides otherwise, and no one-shot has a length that can express that.
void vg_synth_flatline(bool on);

// Fill `n` mono samples, mixing every live voice and the engine. `mix` is the
// player's setting, already squared.
void vg_synth_render(int16_t* out, int n, float mix);
