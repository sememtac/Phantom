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
    v->lp1 = v->lp2 = 0.0f;
    // One-pole coefficient per stage, at the generation rate.
    const float x = 6.2831853f * lp_hz / (float)VG_AUDIO_RATE;
    v->lp_k = (x > 1.0f) ? 1.0f : x;
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
    // A hard square that holds: this is an instrument telling you something, not
    // a sound the world made. It climbs with `pitch` as the missile closes.
    case SFX_MSL_ALERT:
        voice_set(v, W_SQUARE, 880.0f * pitch, 880.0f * pitch, 0.10f, 0.002f, 0.30f, 6000.0f);
        break;
    // Lower and rougher than the missile, because the wall is the duller death.
    case SFX_WALL_ALERT:
        voice_set(v, W_SQUARE, 300.0f * pitch, 260.0f * pitch, 0.16f, 0.004f, 0.30f, 2600.0f);
        break;
    // A click, which is noise that is over before it is a sound.
    case SFX_MSL_EVENT:
        voice_set(v, W_NOISE, 0.0f, 0.0f, 0.030f, 0.001f, 0.35f, 5000.0f);
        break;
    // Two-tone would need two voices; one short blip at the top of the band reads
    // as a radio without spending a second.
    case SFX_COMMS:
        voice_set(v, W_SINE, 1500.0f, 1900.0f, 0.07f, 0.004f, 0.22f, 9000.0f);
        break;
    // Noise falling away: the rail, then the motor leaving.
    case SFX_LAUNCH:
        voice_set(v, W_NOISE, 0.0f, 0.0f, 0.34f, 0.004f, 0.40f, 3000.0f);
        break;
    // The same generator as the click, an order of magnitude longer and with far
    // more taken off the top. Almost all of the character is in the filter.
    case SFX_EXPLODE:
        voice_set(v, W_NOISE, 0.0f, 0.0f, 0.70f, 0.006f, 0.55f, 900.0f);
        break;
    default: v->on = false; break;
    }
}

void vg_sfx_update(void) {
    int room = vg_audio_room();
    if (room <= 0) return;
    if (room > 512) room = 512;          // a frame's worth is ~368; cap the burst

    static int16_t buf[512];
    const float dt = 1.0f / (float)VG_AUDIO_RATE;

    // The player's setting, applied once here rather than per voice. Squared,
    // because a linear volume slider spends most of its travel doing very little
    // -- loudness is not linear and a slider that behaves as if it were feels
    // broken at the bottom.
    const float mix = vg.vol_sfx * vg.vol_sfx;

    for (int n = 0; n < room; n++) {
        float acc = 0.0f;

        for (int i = 0; i < VOICES; i++) {
            Voice* v = &s_v[i];
            if (!v->on) continue;

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
