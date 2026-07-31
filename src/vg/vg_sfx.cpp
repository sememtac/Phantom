#include "vg_sfx.h"
#include "vg_port.h"
#include "vg_game.h"
#include <Arduino.h>
#include <math.h>

// A voice is an oscillator, an amplitude envelope and a pitch sweep. That is the
// whole synth, and it covers every cue the game asks for: a beep is a square that
// holds, a click is noise that stops immediately, an explosion is noise that
// takes a second to stop, a launch is noise plus a falling tone.

enum Wave : unsigned char { W_SQUARE = 0, W_NOISE, W_SINE };

struct Voice {
    bool  on;
    Wave  wave;
    float phase;        // 0..1
    float freq;         // Hz, current
    float freq_to;      // Hz, swept toward over the life of the voice
    float t, life;      // seconds
    float attack;       // seconds to full
    float gain;
    float delay;        // seconds before it starts, for two-tone cues
    // Amplitude modulation. What turns a plain square into something reedy --
    // the difference between a tone and a quack is almost entirely this.
    float mod_hz, mod_phase, mod_depth;
    // Two-pole low pass, which is what stops noise sounding like a burst of
    // static. An explosion is the same generator as a click; the difference is
    // almost entirely what is taken off the top of it.
    float lp1, lp2, lp_k;
};

// Four is enough and the ceiling matters: alerts repeat, and an unbounded mixer
// would let a boundary warning and a missile warning and a comms beep stack into
// something louder than any of them was designed to be.
#define VOICES 4
static Voice s_v[VOICES];

static uint32_t s_rng = 0x1234567u;   // NOT the game's. See the note in the header.
static inline float noise(void) {
    s_rng ^= s_rng << 13; s_rng ^= s_rng >> 17; s_rng ^= s_rng << 5;
    return (float)(int32_t)s_rng * (1.0f / 2147483648.0f);
}

static Voice* grab(void) {
    // A free voice, or the one with least life left. Stealing the oldest is what
    // keeps a repeating alert from being cut off by its own next beep.
    Voice* best = nullptr;
    float  worst = 1e9f;
    for (int i = 0; i < VOICES; i++) {
        if (!s_v[i].on) return &s_v[i];
        const float left = s_v[i].life - s_v[i].t;
        if (left < worst) { worst = left; best = &s_v[i]; }
    }
    return best;
}

static void voice_set(Voice* v, Wave w, float f0, float f1, float life,
                      float attack, float gain, float lp_hz) {
    v->on = true; v->wave = w; v->phase = 0.0f;
    v->freq = f0; v->freq_to = f1;
    v->t = 0.0f; v->life = life; v->attack = attack; v->gain = gain;
    v->delay = 0.0f;
    v->mod_hz = 0.0f; v->mod_phase = 0.0f; v->mod_depth = 0.0f;
    v->lp1 = v->lp2 = 0.0f;
    // One-pole coefficient per stage, at the generation rate.
    const float x = 6.2831853f * lp_hz / (float)VG_AUDIO_RATE;
    v->lp_k = (x > 1.0f) ? 1.0f : x;
}

// ---------------------------------------------------------------------------
// The airframe
//
// Not a cue and not in the voice pool: it is on for as long as the ship is
// flying, and a one-shot that had to be retriggered would either gap or overlap
// itself. It is also the only sound here the player hears CONTINUOUSLY, which
// sets every other decision about it -- quiet, dull, and with no edge to catch
// on, because a hum you notice after ten seconds is a hum you hate after five
// minutes.
//
// Two detuned saws an octave apart plus a little filtered noise. The detune is
// what stops it sounding like a test tone: two oscillators a few Hz apart beat
// against each other slowly, which reads as machinery rather than as an
// oscillator.
// ---------------------------------------------------------------------------
static float s_eng_lvl   = 0.0f;    // smoothed, so the throttle does not step
static float s_eng_p1    = 0.0f, s_eng_p2 = 0.0f;
static float s_eng_lp    = 0.0f;
static float s_eng_want  = 0.0f;
static bool  s_eng_on    = false;

void vg_sfx_engine(bool on, float throttle) {
    s_eng_on = on;
    if (throttle < 0.0f) throttle = 0.0f;
    if (throttle > 1.0f) throttle = 1.0f;
    // Idle is audible but only just; the top of the travel is where it should be
    // obvious that the airframe is working, which is the same thing the buzz and
    // the roll authority are saying at that end of the slider.
    s_eng_want = on ? (0.16f + 0.84f * throttle) : 0.0f;
}

bool vg_sfx_init(void) {
    for (int i = 0; i < VOICES; i++) s_v[i].on = false;
    return vg_audio_init();
}

void vg_sfx_play(SfxId id, float pitch) {
    if (pitch < 0.25f) pitch = 0.25f;
    if (pitch > 4.0f)  pitch = 4.0f;

    Voice* v = grab();
    if (!v) return;

    switch (id) {
    // THE MISSILE. Low, reedy and unmistakable -- the quack a lock warning makes
    // in every science fiction cockpit anyone has sat in. It is a plain square
    // that would be a dull beep on its own; the whole character is the tremolo
    // chopping it at 42 Hz, fast enough to hear as timbre rather than as pulses.
    //
    // Fixed pitch. Always. See the note in the header.
    case SFX_MSL_ALERT:
        voice_set(v, W_SQUARE, 300.0f, 300.0f, 0.17f, 0.002f, 0.34f, 2200.0f);
        v->mod_hz = 42.0f; v->mod_depth = 0.85f;
        break;

    // THE BOUNDARY, as a two-tone: high then low, the shape aviation uses for
    // "pull up" and the reason it is legible through everything else happening.
    // Two voices, the second delayed by the first's length -- a single voice
    // stepping its own frequency would slur between the tones instead of
    // articulating them, and the articulation is the whole point.
    case SFX_WALL_ALERT: {
        voice_set(v, W_SQUARE, 760.0f, 760.0f, 0.13f, 0.003f, 0.30f, 3600.0f);
        Voice* lo = grab();
        if (lo && lo != v) {
            voice_set(lo, W_SQUARE, 505.0f, 505.0f, 0.17f, 0.003f, 0.30f, 3000.0f);
            lo->delay = 0.135f;
        }
        break;
    }

    // A click, which is noise that is over before it is a sound.
    case SFX_MSL_EVENT:
        voice_set(v, W_NOISE, 0.0f, 0.0f, 0.030f, 0.001f, 0.35f, 5000.0f);
        break;

    // Two-tone would need two voices; one short blip at the top of the band reads
    // as a radio without spending a second.
    case SFX_COMMS:
        voice_set(v, W_SINE, 1500.0f, 1900.0f, 0.07f, 0.004f, 0.22f, 9000.0f);
        break;

    // LOWER. A rocket leaving the rail is felt more than heard, so most of this
    // is a falling tone with the noise sitting underneath it rather than on top.
    case SFX_LAUNCH: {
        voice_set(v, W_NOISE, 0.0f, 0.0f, 0.38f, 0.004f, 0.30f, 1300.0f);
        Voice* tone = grab();
        if (tone && tone != v)
            voice_set(tone, W_SQUARE, 165.0f, 62.0f, 0.34f, 0.006f, 0.34f, 1100.0f);
        break;
    }

    // The same generator as the click, an order of magnitude longer and with far
    // more taken off the top. Almost all of the character is in the filter.
    case SFX_EXPLODE:
        voice_set(v, W_NOISE, 0.0f, 0.0f, 0.70f, 0.006f, 0.55f, 900.0f);
        break;

    default: v->on = false; break;
    }
}

void vg_sfx_update(void) {
    int room = vg_audio_due();
    if (room <= 0) return;
    if (room > 512) room = 512;          // a frame's worth is ~368; cap the burst

    static int16_t buf[512];
    const float dt = 1.0f / (float)VG_AUDIO_RATE;

    // The player's setting, applied once here rather than per voice. Squared,
    // because a linear volume slider spends most of its travel doing very little
    // -- loudness is not linear and a slider that behaves as if it were feels
    // broken at the bottom.
    const float mix = vg.vol_sfx * vg.vol_sfx;

    // Per-sample smoothing constant for the engine level. Slow enough that
    // slamming the throttle is a swell rather than a step.
    const float eng_k = 1.0f - expf(-dt * 3.0f);

    for (int n = 0; n < room; n++) {
        float acc = 0.0f;

        // --- the airframe --------------------------------------------------
        s_eng_lvl += (s_eng_want - s_eng_lvl) * eng_k;
        if (s_eng_lvl > 0.0005f) {
            // Frequency rides the same level, so it climbs as well as swells.
            const float f = 46.0f + 34.0f * s_eng_lvl;
            s_eng_p1 += f * dt;
            s_eng_p2 += (f * 2.006f) * dt;      // an octave, detuned a hair
            if (s_eng_p1 >= 1.0f) s_eng_p1 -= (float)(int)s_eng_p1;
            if (s_eng_p2 >= 1.0f) s_eng_p2 -= (float)(int)s_eng_p2;

            const float saw1 = s_eng_p1 * 2.0f - 1.0f;
            const float saw2 = s_eng_p2 * 2.0f - 1.0f;
            float e = saw1 * 0.6f + saw2 * 0.25f + noise() * 0.14f;

            // One pole, low. Everything above a few hundred Hz in a saw is what
            // makes it a buzz instead of a hum.
            s_eng_lp += (e - s_eng_lp) * 0.06f;
            // Louder than it was. At 0.22 it was technically present and
            // practically inaudible under everything else -- an engine you have
            // to listen for is not doing the job an engine is there to do.
            acc += s_eng_lp * s_eng_lvl * 0.55f;
        }

        for (int i = 0; i < VOICES; i++) {
            Voice* v = &s_v[i];
            if (!v->on) continue;

            // Waiting its turn: the second half of a two-tone.
            if (v->delay > 0.0f) { v->delay -= dt; continue; }

            const float u = v->t / v->life;
            if (u >= 1.0f) { v->on = false; continue; }

            // Attack then decay. The attack is short and exists only to stop the
            // click that starting a waveform at full amplitude would make -- a
            // click that would be audible on every single cue.
            float env = (v->t < v->attack) ? (v->t / v->attack) : (1.0f - u);
            if (env < 0.0f) env = 0.0f;
            env *= env;                     // fall away faster than linearly

            float sample;
            if (v->wave == W_NOISE) {
                sample = noise();
            } else {
                v->phase += v->freq * dt;
                if (v->phase >= 1.0f) v->phase -= (float)(int)v->phase;
                sample = (v->wave == W_SQUARE) ? ((v->phase < 0.5f) ? 1.0f : -1.0f)
                                               : sinf(v->phase * 6.2831853f);
            }

            // Tremolo, if this cue has one. Chopping the amplitude fast is what
            // makes a square reedy rather than merely loud.
            if (v->mod_hz > 0.0f) {
                v->mod_phase += v->mod_hz * dt;
                if (v->mod_phase >= 1.0f) v->mod_phase -= (float)(int)v->mod_phase;
                const float m = (v->mod_phase < 0.5f) ? 1.0f : (1.0f - v->mod_depth);
                env *= m;
            }

            // Two one-pole stages in series.
            v->lp1 += (sample - v->lp1) * v->lp_k;
            v->lp2 += (v->lp1  - v->lp2) * v->lp_k;
            acc += v->lp2 * env * v->gain;

            v->freq += (v->freq_to - v->freq) * (dt / v->life);
            v->t    += dt;
        }

        acc *= mix;
        if (acc >  1.0f) acc =  1.0f;       // clip rather than wrap
        if (acc < -1.0f) acc = -1.0f;
        buf[n] = (int16_t)(acc * 30000.0f);
    }

    vg_audio_write(buf, room);
}
