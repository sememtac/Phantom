#include "vg_synth.h"
#include "vg_port.h"
#include "vg_prof.h"
#include <math.h>

float    g_synth_peak    = 0.0f;
uint32_t g_synth_knee    = 0;

// NOT the game's random stream. Drawing from that would make the audio a term in
// the simulation and take replay determinism with it.
static uint32_t s_rng = 0x1234567u;
static inline float noise(void) {
    s_rng ^= s_rng << 13; s_rng ^= s_rng >> 17; s_rng ^= s_rng << 5;
    return (float)(int32_t)s_rng * (1.0f / 2147483648.0f);
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

void vg_synth_engine(bool on, float throttle) {
    s_eng_on = on;
    if (throttle < 0.0f) throttle = 0.0f;
    if (throttle > 1.0f) throttle = 1.0f;
    // Idle is audible but only just; the top of the travel is where it should be
    // obvious that the airframe is working, which is the same thing the buzz and
    // the roll authority are saying at that end of the slider.
    s_eng_want = on ? (0.16f + 0.84f * throttle) : 0.0f;
}


// ---------------------------------------------------------------------------
// The flatline
//
// A monitor with nothing left to report. Steady, unwavering and slightly harsh:
// the point of it is that it does not develop, because the thing it is reporting
// is not going to change.
//
// Ramped on and off rather than switched. A tone that starts at full amplitude
// clicks, and a click at the front of this particular sound would be the funniest
// possible mistake.
// ---------------------------------------------------------------------------
static float s_flat_lvl  = 0.0f;
static float s_flat_want = 0.0f;
static float s_flat_ph   = 0.0f;

void vg_synth_flatline(bool on) { s_flat_want = on ? 1.0f : 0.0f; }

// ---------------------------------------------------------------------------
// Voices
// ---------------------------------------------------------------------------

struct Voice {
    bool  on;
    SynthWave wave;
    float phase;
    float freq, freq_to;
    float t, life;
    float attack, sustain, gain, delay;
    float mod_hz, mod_phase, mod_depth;
    float lp1, lp2, lp_k;
};

// Ten. Entering the course asks for the broadcast's four-note jingle, the
// panel's four-note ready cue and the set's two-part turn-on inside a single
// frame; a hull hit alone is seven. Ten is the busiest real moment plus a
// little, not a number chosen to stop thinking about it.
//
// The ceiling still matters: alerts repeat, and an unbounded mixer would let a
// boundary warning and a missile warning and a comms beep stack into something
// louder than any of them was designed to be.
#define VOICES 10
static Voice s_v[VOICES];

static Voice* grab(void) {
    // A free voice, or the one with least left to do.
    //
    // THE DELAY COUNTS. It did not, and that is why the broadcast's jingle never
    // sounded at the start of the course: four notes queued with delays and a
    // tenth of a second of life each looked like the cheapest voices in the
    // mixer, so the ready cue -- fired one line later in the same frame -- took
    // all four before the first had played. A note waiting its turn has not had
    // its turn.
    Voice* best = nullptr;
    float  worst = 1e9f;
    for (int i = 0; i < VOICES; i++) {
        if (!s_v[i].on) return &s_v[i];
        const float left = s_v[i].delay + (s_v[i].life - s_v[i].t);
        if (left < worst) { worst = left; best = &s_v[i]; }
    }
    return best;
}

int vg_synth_live(void) {
    int n = 0;
    for (int i = 0; i < VOICES; i++) if (s_v[i].on) n++;
    return n;
}

void vg_synth_reset(void) {
    for (int i = 0; i < VOICES; i++) s_v[i].on = false;
}

void vg_synth_silence(void) {
    // Everything, and at once. vg_synth_reset only frees the voices, and the two
    // held sounds are not voices: the engine smooths towards its target and the
    // flatline ramps, both of which are right when a ship stops flying and wrong
    // when the picture has already gone. Their levels go to zero here rather
    // than their targets.
    for (int i = 0; i < VOICES; i++) s_v[i].on = false;
    s_eng_on   = false;
    s_eng_want = 0.0f;
    s_eng_lvl  = 0.0f;
    s_flat_want = 0.0f;
    s_flat_lvl  = 0.0f;
}

void vg_synth_layer(const SynthLayer* l, float pitch) {
    Voice* v = grab();
    if (!v) return;

    v->on      = true;
    v->wave    = l->wave;
    v->phase   = 0.0f;
    v->freq    = l->f0 * pitch;
    v->freq_to = l->f1 * pitch;
    v->t       = 0.0f;
    v->life    = l->life;
    v->attack  = l->attack;
    v->sustain = l->sustain;
    v->gain    = l->gain;
    v->delay   = l->delay;
    v->mod_hz  = l->mod_hz;
    v->mod_phase = 0.0f;
    v->mod_depth = l->mod_depth;
    v->lp1 = v->lp2 = 0.0f;

    const float x = 6.2831853f * l->lp_hz / (float)VG_AUDIO_RATE;
    v->lp_k = (x > 1.0f) ? 1.0f : x;
}

// Sine by smoothed parabola, phase in [0,1). Accurate to about a tenth of a
// percent, which on a one-centimetre speaker playing alarm beeps is exact.
// It exists because the mixer pays for sine PER SAMPLE PER VOICE: profiling
// found the whole synth billed to the submit stage at up to several
// milliseconds a frame, sustained, whenever alarms stacked -- and the library
// sinf was most of it.
static inline float fsin01(float x) {
    x -= (float)(int)x;
    const bool  neg = (x >= 0.5f);
    const float t   = neg ? (x - 0.5f) : x;
    const float par = 16.0f * t * (0.5f - t);          // parabola, peak 1
    const float y   = 0.775f * par + 0.225f * par * par;
    return neg ? -y : y;
}

// THE RAIL, SOFTENED.
//
// It was a hard clamp, and the comment on it said "clip rather than wrap" -- which
// was the right call against wrapping and stopped one step short. A collision on
// the course stacks a hull hit (seven layers, one of them at gain 1.00) on an
// explosion (noise at 0.80), and the sum was measured at 1.94: two hundred
// milliseconds of flat-topped waveform inside a two-second window. On a mix made
// mostly of squares, flattening the tops is not a limit, it is a crunch, and that
// crunch is what a collision sounded like.
//
// Linear below the knee, then asymptotic to 1.0. Two properties earn their keep:
//
//   The knee sits at 0.8, ABOVE EVERYTHING NORMAL PLAY DOES. Measured: quiet
//   windows peak at 0.61 to 0.75. So the ordinary sound of the game passes through
//   this untouched, bit for bit -- which matters more than the curve's shape does,
//   because every number in the cue catalogue was tuned by ear against the old
//   behaviour and a knee that moved the quiet levels would retune all of them at
//   once without saying so.
//
//   The slope is continuous at the knee: the derivative of e/(1+e) at e=0 is 1,
//   which is exactly the slope of the line below it. No corner, so nothing new to
//   hear at the crossing.
//
// The asymptote also means the output cannot reach 1.0, so there is no clamp after
// this and no way to wrap.
#define SYNTH_KNEE 0.8f
static inline float soft_rail(float x) {
    const float m = x < 0.0f ? -x : x;
    if (m <= SYNTH_KNEE) return x;
    const float e = (m - SYNTH_KNEE) / (1.0f - SYNTH_KNEE);
    const float y = SYNTH_KNEE + (1.0f - SYNTH_KNEE) * (e / (1.0f + e));
    return x < 0.0f ? -y : y;
}

void vg_synth_render(int16_t* out, int n_out, float mix) {
    const int   room = n_out;
    int16_t*    buf  = out;
    const float dt   = 1.0f / (float)VG_AUDIO_RATE;

    // Per-sample smoothing constant for the engine level. Slow enough that
    // slamming the throttle is a swell rather than a step.
    const float eng_k  = 1.0f - expf(-dt * 3.0f);
    const float flat_k = 1.0f - expf(-dt * 1.1f);

    for (int n = 0; n < room; n++) {
        float acc = 0.0f;

        // --- somebody is dead ----------------------------------------------
        // Slower than the engine's ramp, deliberately: the tone should emerge
        // from under the static rather than start with it. Roughly a second to
        // settle, which is about as long as the noise in front of it lasts.
        s_flat_lvl += (s_flat_want - s_flat_lvl) * flat_k;
        // SNAPPED TO ZERO, not allowed to decay forever. An exponential decay
        // spends half a minute crossing the subnormal range, and this FPU
        // handles subnormals in software -- hundreds of cycles per operation,
        // per sample. The profiler caught the mixer at 4.7ms a frame in the
        // seconds after leaving flight, which is exactly when these levels are
        // decaying through the storm. Below audibility is zero.
        if (s_flat_want == 0.0f && s_flat_lvl < 1e-5f) s_flat_lvl = 0.0f;
        if (s_flat_lvl > 0.0005f) {
            s_flat_ph += 988.0f * dt;
            if (s_flat_ph >= 1.0f) s_flat_ph -= (float)(int)s_flat_ph;
            // Mostly sine with a little square in it. A pure tone is a test
            // signal; the edge is what makes it an instrument in a room.
            const float sn = fsin01(s_flat_ph);
            const float sq = (s_flat_ph < 0.5f) ? 1.0f : -1.0f;
            // QUIET. 0.26 down to 0.115 -- this is not an alarm demanding
            // anything, it is a reading nobody is going to act on, and a faint
            // one you have to notice is worse to sit with than a loud one you
            // are being told to react to.
            acc += (sn * 0.82f + sq * 0.18f) * s_flat_lvl * 0.115f;
        }

        // --- the airframe --------------------------------------------------
        s_eng_lvl += (s_eng_want - s_eng_lvl) * eng_k;
        if (s_eng_want == 0.0f && s_eng_lvl < 1e-5f) s_eng_lvl = 0.0f;
        if (s_eng_lvl > 0.0005f) {
            // A DRIVE, NOT A PROPELLER. Three things were making it an aircraft:
            // the fundamental sat at 46-80 Hz, a second oscillator an OCTAVE
            // above it added the brightness a prop has, and there was enough
            // noise on top to read as air being moved. All three are the sound of
            // something pushing against an atmosphere.
            //
            // Now: half the frequency, the second oscillator moved from an octave
            // up to a hair off UNISON, and most of the noise gone.
            const float f = 29.0f + 21.0f * s_eng_lvl;
            s_eng_p1 += f * dt;
            // 1.006, so the two beat against each other about every three
            // seconds. Slow enough to read as something enormous idling rather
            // than as two oscillators disagreeing.
            s_eng_p2 += (f * 1.006f) * dt;
            if (s_eng_p1 >= 1.0f) s_eng_p1 -= (float)(int)s_eng_p1;
            if (s_eng_p2 >= 1.0f) s_eng_p2 -= (float)(int)s_eng_p2;

            const float saw1 = s_eng_p1 * 2.0f - 1.0f;
            const float saw2 = s_eng_p2 * 2.0f - 1.0f;
            float e = saw1 * 0.55f + saw2 * 0.45f + noise() * 0.05f;

            // Deliberately NOT filtered harder to match the lower note. This
            // speaker is a centimetre across and reproduces nothing near 30 Hz --
            // the pitch the player hears is inferred from the harmonics, so
            // taking those off would make it quieter without making it lower.
            s_eng_lp += (e - s_eng_lp) * 0.055f;
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
            float env;
            if (v->t < v->attack) {
                env = v->t / v->attack;
            } else if (u < v->sustain) {
                env = 1.0f;                 // held: the part that is not a beat
            } else {
                const float d = 1.0f - v->sustain;
                env = (d > 0.0001f) ? (1.0f - (u - v->sustain) / d) : (1.0f - u);
            }
            if (env < 0.0f) env = 0.0f;
            env *= env;                     // fall away faster than linearly

            float sample;
            if (v->wave == SW_NOISE) {
                sample = noise();
            } else {
                v->phase += v->freq * dt;
                if (v->phase >= 1.0f) v->phase -= (float)(int)v->phase;
                sample = (v->wave == SW_SQUARE) ? ((v->phase < 0.5f) ? 1.0f : -1.0f)
                                               : fsin01(v->phase);
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
        // Headroom, measured before the knee gets a chance to hide it. See
        // vg_prof.h. This is the honest peak: what the voices actually summed to.
        {
            const float mag = acc < 0.0f ? -acc : acc;
            if (mag > g_synth_peak)   g_synth_peak = mag;
            if (mag > SYNTH_KNEE)     g_synth_knee++;
        }
        acc = soft_rail(acc);
        buf[n] = (int16_t)(acc * 30000.0f);
    }

}
