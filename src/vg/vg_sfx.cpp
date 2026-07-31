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
    // Fraction of the life HELD at full before the decay starts. Zero is the
    // shape everything had until now -- rise, then fall away immediately -- which
    // is right for a click and wrong for anything meant to read as sustained
    // damage. A single decaying beat is an event; something that holds and then
    // gives way is a thing failing.
    float sustain;
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

// Ten. Six was not enough for the moment the course begins, which fires the
// broadcast's four-note jingle, the panel's four-note ready cue and the set's
// two-part turn-on inside a single frame -- ten voices asked for, six available,
// and the loser was whichever had least left to do.
//
// The ceiling still matters: alerts repeat, and an unbounded mixer would let a
// boundary warning and a missile warning and a comms beep stack into something
// louder than any of them was designed to be. Ten is the busiest real moment
// plus a little, not a number chosen to stop thinking about it.
#define VOICES 10
static Voice s_v[VOICES];

static uint32_t s_rng = 0x1234567u;   // NOT the game's. See the note in the header.
static inline float noise(void) {
    s_rng ^= s_rng << 13; s_rng ^= s_rng >> 17; s_rng ^= s_rng << 5;
    return (float)(int32_t)s_rng * (1.0f / 2147483648.0f);
}

static Voice* grab(void) {
    // A free voice, or the one with least left to do.
    //
    // THE DELAY COUNTS. It did not, and that is why the broadcast's jingle never
    // sounded at the start of the course: the four notes of it are queued with
    // delays and a life of a tenth of a second each, so by this measure they
    // looked like the most expendable voices in the mixer -- and the panel's
    // ready cue, fired one line later in the same frame, took all four before the
    // first had played. A note waiting its turn has not had its turn.
    Voice* best = nullptr;
    float  worst = 1e9f;
    for (int i = 0; i < VOICES; i++) {
        if (!s_v[i].on) return &s_v[i];
        const float left = s_v[i].delay + (s_v[i].life - s_v[i].t);
        if (left < worst) { worst = left; best = &s_v[i]; }
    }
    return best;
}

static void voice_set(Voice* v, Wave w, float f0, float f1, float life,
                      float attack, float gain, float lp_hz) {
    v->on = true; v->wave = w; v->phase = 0.0f;
    v->freq = f0; v->freq_to = f1;
    v->t = 0.0f; v->life = life; v->attack = attack; v->gain = gain;
    v->sustain = 0.0f;
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

    // A click, which is noise that is over before it is a sound. Dropped from
    // 5 kHz to 2.4 kHz: it was a tick off a desk toy, and this is a weapon
    // reporting. Still short enough to be punctuation rather than an event.
    case SFX_MSL_EVENT:
        voice_set(v, W_NOISE, 0.0f, 0.0f, 0.034f, 0.001f, 0.42f, 2400.0f);
        break;

    // Two-tone would need two voices; one short blip at the top of the band reads
    // as a radio without spending a second.
    case SFX_COMMS:
        voice_set(v, W_SINE, 1500.0f, 1900.0f, 0.07f, 0.004f, 0.22f, 9000.0f);
        break;

    // LOWER. A rocket leaving the rail is felt more than heard, so most of this
    // is a falling tone with the noise sitting underneath it rather than on top.
    // DEEP AND GROWLING, and long enough to be a departure rather than a click.
    // A rocket leaving the rail is a sustained thing -- the motor keeps burning
    // after the round has gone -- so this holds like the hull cue does, with the
    // same judder on it an octave up in modulation rate.
    case SFX_LAUNCH: {
        voice_set(v, W_SQUARE, 104.0f, 34.0f, 0.80f, 0.004f, 0.60f, 220.0f);
        v->sustain = 0.25f;
        v->mod_hz  = 19.0f; v->mod_depth = 0.5f;
        Voice* wash = grab();
        if (wash && wash != v) {
            // The motor, under the tone rather than over it. 1300 Hz put the
            // whole cue up where the speaker is bright and made it a hiss.
            voice_set(wash, W_NOISE, 0.0f, 0.0f, 0.60f, 0.006f, 0.30f, 420.0f);
            wash->sustain = 0.20f;
        }
        break;
    }

    // LOWER AND LOUDER. A tournament where the losing pilot is heard dying
    // should not resolve its deaths with a polite hiss. 900 Hz down to 420 and
    // the gain up by half -- the cue that most wants to be felt rather than
    // noticed.
    //
    // A second voice under it, a tone falling to almost nothing, which is the
    // part a small speaker turns into weight. Noise alone at this cutoff is a
    // shhh; the falling tone is what makes it a detonation.
    case SFX_EXPLODE: {
        voice_set(v, W_NOISE, 0.0f, 0.0f, 0.85f, 0.004f, 0.80f, 420.0f);
        Voice* thud = grab();
        if (thud && thud != v)
            voice_set(thud, W_SQUARE, 90.0f, 28.0f, 0.55f, 0.004f, 0.55f, 700.0f);
        break;
    }

    // THE HULL TAKING IT, and it GROWLS. The heaviest thing in the game: the
    // player's own ship being hurt should land like structure giving way, not
    // like a scoring event.
    //
    // Third attempt at this, and the first two were wrong about WHY it sounded
    // high. Dropping the fundamental did almost nothing, because the speaker on
    // this board reproduces very little below a few hundred Hz -- the note itself
    // was never what was being heard. What was being heard is the material above
    // it: a noise burst at 1600 Hz, then at 700, sitting where the speaker is
    // most efficient and telling the ear "small and sharp" no matter what the
    // tone underneath was doing.
    //
    // So the noise is nearly gone, everything is filtered to 240 Hz, and the
    // growl is carried by a SLOW tremolo -- 13 Hz, which is under the rate the
    // ear fuses into timbre, so it is heard as a thing juddering rather than as
    // a buzz. Longer, too: structure fails over time, and 0.5s was an impact.
    case SFX_HIT: {
        // 240 Hz down to 130. This is the number that decides what actually
        // leaves the speaker, and it is worth being blunt about the cost: every
        // step down here trades loudness for depth, because it is taking away
        // the harmonics the driver is efficient at and keeping the ones it is
        // not. Quieter and lower is the deal on a speaker this size; there is no
        // setting that is both.
        // A SECOND AND A HALF, and a third of it held. The pitch was right and the
        // shape was not: one decaying beat is an impact, and this is supposed to
        // be the hull failing -- something that keeps happening after it starts.
        // The judder runs through the whole of it, which is what a held note
        // buys that a decaying one cannot.
        // PITCHED AGAINST THE ENGINE, deliberately: the drive sits at 29-50 Hz
        // under a 198 Hz filter, and this sits just above both. Chasing it ever
        // downward was wrong -- at 26 Hz under a 92 Hz filter almost nothing was
        // leaving the driver, and what did was the beeps, which is why the cue
        // kept being reported as high no matter how far the note fell.
        //
        // Two seconds now. It is meant to outlast the moment that caused it.
        voice_set(v, W_SQUARE, 66.0f, 28.0f, 2.00f, 0.004f, 1.00f, 240.0f);
        v->sustain = 0.40f;
        v->mod_hz  = 11.0f; v->mod_depth = 0.75f;
        Voice* rasp = grab();
        if (rasp && rasp != v) {
            voice_set(rasp, W_NOISE, 0.0f, 0.0f, 0.60f, 0.002f, 0.16f, 300.0f);
            rasp->sustain = 0.30f;
            rasp->mod_hz  = 11.0f; rasp->mod_depth = 0.6f;
        }

        // AND THE PANEL SHOUTING ABOUT IT. Four quick high beeps, each quieter
        // than the last, over the top of the groan.
        //
        // This is the contrast doing the work rather than the depth alone. A low
        // sound on its own has nothing to be low against, and on a speaker this
        // small the bottom end is barely there -- so what sells the weight is
        // something bright and thin sitting above it and losing. The systems
        // reporting damage while the airframe answers underneath.
        // 1320 Hz was too high and too close together: it did not read as four
        // beeps over a groan, it read as the whole cue being bright. 700 Hz and
        // nearly three times the spacing, so they are heard as separate events
        // sitting above the airframe rather than as part of its timbre.
        static const float beep_at[4] = { 0.06f, 0.24f, 0.42f, 0.60f };
        static const float beep_g [4] = { 0.32f, 0.24f, 0.16f, 0.09f };
        for (int i = 0; i < 4; i++) {
            Voice* b = grab();
            if (!b || b == v) break;
            voice_set(b, W_SQUARE, 700.0f, 700.0f, 0.07f, 0.002f,
                      beep_g[i], 3200.0f);
            b->delay = beep_at[i];
        }
        break;
    }

    // A TICK, THEN A LOW ZAP. The tick is the relay; the zap is the tube.
    //
    // The static went the way the sine did. Broadband hiss reads as a fault --
    // an untuned channel, something wrong -- and this transition is the set
    // working correctly. A tick is a mechanism operating, and a zap that lives
    // down in the low register is a lot of energy moving without being musical
    // about it.
    case SFX_TV_ON: {
        voice_set(v, W_NOISE, 0.0f, 0.0f, 0.014f, 0.0005f, 0.70f, 3500.0f);
        Voice* zap = grab();
        if (zap && zap != v) {
            // Upward, because the picture is opening. Kept under 620 Hz: a zap
            // that climbs into the top of the band is a laser, not a tube.
            voice_set(zap, W_SQUARE, 95.0f, 610.0f, 0.13f, 0.001f, 0.45f, 900.0f);
            zap->delay = 0.020f;
        }
        break;
    }

    // Off is the same two events in the same order -- tick, then zap -- with the
    // zap running down as the picture collapses. Not reversed: the relay comes
    // first either way, because the mechanism acts and the tube answers.
    case SFX_TV_OFF: {
        voice_set(v, W_NOISE, 0.0f, 0.0f, 0.014f, 0.0005f, 0.70f, 3500.0f);
        Voice* zap = grab();
        if (zap && zap != v) {
            voice_set(zap, W_SQUARE, 580.0f, 80.0f, 0.15f, 0.001f, 0.45f, 900.0f);
            zap->delay = 0.020f;
        }
        break;
    }

    // SYSTEMS ONLINE. Three rising tones over a low one, which is the only cue
    // in the game that is a SEQUENCE rather than a sound -- because it is not
    // reporting an event, it is a machine finishing something, and finishing
    // takes steps.
    //
    // Squares rather than sines: this is the instrument talking, in the same
    // voice as the warnings, and it should be recognisably the same panel.
    // THE BROADCAST, ABOUT TO SPEAK. Two tones, twice -- the shape a public
    // address system uses before it tells you something, and recognisable as
    // "listen" long before anybody reads the words.
    //
    // Deliberately the HIGHEST thing in the mix. Everything else has been pushed
    // down for the tournament's weight; this one has to come over the top of all
    // of it, because it is the only voice in the game that is not in the room.
    // `pitch` below 1 asks for the SHORT form: one pair instead of two. Every
    // line the broadcast speaks is announced, but a three-line briefing that
    // played the full double beat three times would be three announcements as
    // far as the ear is concerned. The opener gets the whole thing; the lines
    // that continue it get the tail of it.
    case SFX_IFT: {
        const int pairs = (pitch < 1.0f) ? 2 : 4;
        static const float note[4] = { 1046.0f, 740.0f, 1046.0f, 740.0f };
        static const float when[4] = { 0.00f,   0.11f,  0.30f,   0.41f  };
        for (int i = 0; i < pairs; i++) {
            Voice* n = (i == 0) ? v : grab();
            if (!n) break;
            if (i > 0 && n == v) break;
            voice_set(n, W_SQUARE, note[i], note[i], 0.10f, 0.004f, 0.30f, 5200.0f);
            n->delay = when[i];
        }
        break;
    }

    // HELD BACK HALF A SECOND. Entering the course sets the panel booting inside
    // the transition's join, one frame before the set strikes -- so without this
    // the two cues land together and neither is heard. The delay lets the tube
    // finish arriving and then the panel reports in, which is the order the
    // player is watching anyway.
    case SFX_READY: {
        const float t0 = 0.45f;
        voice_set(v, W_SQUARE, 62.0f, 44.0f, 0.55f, 0.006f, 0.40f, 500.0f);
        v->delay = t0;
        static const float note[3] = { 392.0f, 523.0f, 784.0f };
        static const float when[3] = { 0.02f,  0.15f,  0.28f  };
        for (int i = 0; i < 3; i++) {
            Voice* n = grab();
            if (!n || n == v) break;
            voice_set(n, W_SQUARE, note[i], note[i], (i == 2) ? 0.26f : 0.12f,
                      0.004f, 0.26f, 4200.0f);
            n->delay = t0 + when[i];
        }
        break;
    }

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
