#include "vg_sfx.h"
#include <Arduino.h>
#include "vg_synth.h"
#include "vg_port.h"
#include "vg_game.h"
#include "vg_capture.h"
#include "vg_replay.h"

// ===========================================================================
// THE CATALOGUE
//
// What the game's sounds ARE. How they are made is vg_synth.cpp; this file is a
// table, so that adding a cue is adding data and retuning one is editing a
// number in the row that describes it.
//
// It was a switch statement of constructor calls, which worked and hid the thing
// that matters: laid out as rows, the relationships between cues are visible.
// The explosion and the click are the same generator at different lengths. The
// hull cue and the drive are pitched against each other on purpose. Two cues
// share a modulation rate because they are meant to be recognisably the same
// machine complaining.
//
// EVERY NUMBER HERE WAS TUNED BY EAR ON THE ACTUAL SPEAKER, and several are
// counter-intuitive because that speaker is a centimetre across and reproduces
// very little below a few hundred Hz. The lp_hz column decides what leaves the
// driver; f0 mostly decides what the harmonics imply. Chasing a cue "lower" by
// dropping f0 alone does nothing audible, which took four passes on the hull hit
// to learn.
// ===========================================================================

//                wave        f0     f1     life    atk     sus    gain   lp_hz  delay   mod  depth
static const SynthLayer L_MSL_ALERT[] = {
    // The quack every science fiction cockpit has. A plain square would be a dull
    // beep; the character is the 42 Hz tremolo chopping it, fast enough to hear
    // as timbre rather than as pulses. Fixed pitch, always -- an annunciator that
    // moved would be telling the pilot two things with one sound.
    { SW_SQUARE,  300,   300,   0.17f,  0.002f,  0,     0.34f, 2200,  0,      42,  0.85f },
};

static const SynthLayer L_WALL_ALERT[] = {
    // Two-tone, high then low: the shape aviation uses for "pull up". Two layers
    // rather than one sweeping, because a single voice stepping its own frequency
    // slurs between the tones and the articulation is the whole point.
    { SW_SQUARE,  760,   760,   0.13f,  0.003f,  0,     0.30f, 3600,  0,       0,  0 },
    { SW_SQUARE,  505,   505,   0.17f,  0.003f,  0,     0.30f, 3000,  0.135f,  0,  0 },
};

static const SynthLayer L_MSL_EVENT[] = {
    // A click: noise that is over before it is a sound. At 5 kHz it was a tick
    // off a desk toy; this is a weapon reporting.
    { SW_NOISE,     0,     0,   0.034f, 0.001f,  0,     0.42f, 2400,  0,       0,  0 },
};

static const SynthLayer L_COMMS[] = {
    // One blip at the top of the band reads as a radio without spending a second.
    { SW_SINE,   1500,  1900,   0.07f,  0.004f,  0,     0.22f, 9000,  0,       0,  0 },
};

static const SynthLayer L_LAUNCH[] = {
    // Deep and growling, long enough to be a departure rather than a click: a
    // motor keeps burning after the round has gone, so it holds before it falls.
    // The wash sits UNDER the tone -- at 1300 Hz it sat where the driver is
    // brightest and turned the whole cue into a hiss.
    { SW_SQUARE,  104,    34,   0.80f,  0.004f,  0.25f, 0.60f,  220,  0,      19,  0.5f },
    { SW_NOISE,     0,     0,   0.60f,  0.006f,  0.20f, 0.30f,  420,  0,       0,  0 },
};

static const SynthLayer L_HIT[] = {
    // PITCHED AGAINST THE DRIVE, which sits at 29-50 Hz under a 198 Hz filter.
    // This sits just above both, and runs two seconds: damage should outlast the
    // moment that caused it. The 11 Hz judder is what makes it a growl rather
    // than a thump, and it needs the hold to be audible at all.
    { SW_SQUARE,   66,    28,   2.00f,  0.004f,  0.40f, 1.00f,  240,  0,      11,  0.75f },
    { SW_NOISE,     0,     0,   0.60f,  0.002f,  0.30f, 0.16f,  300,  0,      11,  0.6f  },
    // ...and the ship warning over it. DRAWN OUT AND QUACKING, not pipped: at 55
    // milliseconds these were blips, and a blip is a notification. Three times the
    // length with a 36 Hz tremolo on each makes them the same kind of object as
    // the missile alert -- the panel using its warning voice about the hull,
    // rather than the panel making a noise.
    //
    // Fewer of them because they are longer, and still fading, so the sequence
    // reads as something losing its urgency rather than being switched off.
    { SW_SQUARE, 1480,  1480,   0.18f,  0.003f,  0.25f, 0.36f, 5200,  0.05f,  36,  0.8f },
    { SW_SQUARE, 1480,  1480,   0.18f,  0.003f,  0.25f, 0.27f, 5200,  0.30f,  36,  0.8f },
    { SW_SQUARE, 1480,  1480,   0.18f,  0.003f,  0.25f, 0.18f, 5200,  0.55f,  36,  0.8f },
    { SW_SQUARE, 1480,  1480,   0.20f,  0.003f,  0.25f, 0.10f, 5200,  0.80f,  36,  0.8f },
};

static const SynthLayer L_EXPLODE[] = {
    // The same generator as the click with far more taken off the top, plus a
    // tone falling to almost nothing -- noise alone at this cutoff is a shhh, and
    // the falling tone is what a small speaker turns into weight.
    { SW_NOISE,     0,     0,   0.85f,  0.004f,  0,     0.80f,  420,  0,       0,  0 },
    { SW_SQUARE,   90,    28,   0.55f,  0.004f,  0,     0.55f,  700,  0,       0,  0 },
};

static const SynthLayer L_TV_ON[] = {
    // A tick, then a low zap: the relay operating, the tube answering. No tone
    // and no static -- a clean sweep is musical, and broadband hiss reads as a
    // fault, while this transition is the set working correctly.
    { SW_NOISE,     0,     0,   0.014f, 0.0005f, 0,     0.70f, 3500,  0,       0,  0 },
    { SW_SQUARE,   95,   610,   0.13f,  0.001f,  0,     0.45f,  900,  0.02f,   0,  0 },
};

static const SynthLayer L_TV_OFF[] = {
    // The same two events in the same order, the zap running down as the picture
    // collapses. Tick first either way: the mechanism acts, the tube answers.
    { SW_NOISE,     0,     0,   0.014f, 0.0005f, 0,     0.70f, 3500,  0,       0,  0 },
    { SW_SQUARE,  580,    80,   0.15f,  0.001f,  0,     0.45f,  900,  0.02f,   0,  0 },
};

static const SynthLayer L_READY[] = {
    // Systems online: three rising tones over a low one. The only cue that is a
    // SEQUENCE rather than a sound, because it is not reporting an event -- it is
    // a machine finishing something, and finishing takes steps.
    //
    // Held back 0.45s. Entering the course sets the panel booting inside the
    // transition's join, one frame before the set strikes; without the delay both
    // land together and neither is heard.
    { SW_SQUARE,   62,    44,   0.55f,  0.006f,  0,     0.40f,  500,  0.45f,   0,  0 },
    { SW_SQUARE,  392,   392,   0.12f,  0.004f,  0,     0.26f, 4200,  0.47f,   0,  0 },
    { SW_SQUARE,  523,   523,   0.12f,  0.004f,  0,     0.26f, 4200,  0.60f,   0,  0 },
    { SW_SQUARE,  784,   784,   0.26f,  0.004f,  0,     0.26f, 4200,  0.73f,   0,  0 },
};

static const SynthLayer L_IFT[] = {
    // Two tones, twice: what a public address system does before it tells you
    // something. Deliberately the HIGHEST thing in the mix -- everything else has
    // been pushed down for the tournament's weight, and this has to come over the
    // top of it, because it is the only voice in the game not in the room.
    { SW_SQUARE, 1046,  1046,   0.10f,  0.004f,  0,     0.30f, 5200,  0,       0,  0 },
    { SW_SQUARE,  740,   740,   0.10f,  0.004f,  0,     0.30f, 5200,  0.11f,   0,  0 },
    { SW_SQUARE, 1046,  1046,   0.10f,  0.004f,  0,     0.30f, 5200,  0.30f,   0,  0 },
    { SW_SQUARE,  740,   740,   0.10f,  0.004f,  0,     0.30f, 5200,  0.41f,   0,  0 },
};

static const SynthLayer L_IFT_SHORT[] = {
    // One pair. Every line is announced, but a three-line briefing playing the
    // full double beat three times would be three announcements as far as the ear
    // is concerned. The opener gets the whole thing and the lines continuing it
    // get the tail -- the distinction the badge draws visually, drawn again by
    // ear.
    { SW_SQUARE, 1046,  1046,   0.10f,  0.004f,  0,     0.30f, 5200,  0,       0,  0 },
    { SW_SQUARE,  740,   740,   0.10f,  0.004f,  0,     0.30f, 5200,  0.11f,   0,  0 },
};

static const SynthLayer L_DEATH_STATIC[] = {
    // THE SIGNAL GOING. Static first, then the tone settling out from under it --
    // the flatline arriving rather than being switched on, which is the
    // difference between a monitor reporting and a monitor being audible.
    //
    // Two layers because one band of noise is a hiss and two is a transmission
    // failing: something bright breaking up over something duller collapsing.
    { SW_NOISE,     0,     0,   0.55f,  0.010f,  0.20f, 0.34f, 5200,  0,       0,  0 },
    { SW_NOISE,     0,     0,   0.85f,  0.020f,  0.10f, 0.22f, 1100,  0.06f,   0,  0 },
};

struct SfxDef { const SynthLayer* layers; int n; };
#define CUE(a) { a, (int)(sizeof(a) / sizeof((a)[0])) }

// Order must match SfxId. One row per cue, and the compiler checks the count.
static const SfxDef SFX[SFX_COUNT] = {
    CUE(L_MSL_ALERT),
    CUE(L_WALL_ALERT),
    CUE(L_MSL_EVENT),
    CUE(L_COMMS),
    CUE(L_LAUNCH),
    CUE(L_HIT),
    CUE(L_EXPLODE),
    CUE(L_TV_ON),
    CUE(L_TV_OFF),
    CUE(L_READY),
    CUE(L_IFT),
    CUE(L_IFT_SHORT),
    CUE(L_DEATH_STATIC),
};

// ---------------------------------------------------------------------------

bool vg_sfx_init(void) {
    vg_synth_reset();
    return vg_audio_init();
}

void vg_sfx_play(SfxId id, float pitch) {
    if (id >= SFX_COUNT) return;
    if (pitch < 0.25f) pitch = 0.25f;
    if (pitch > 4.0f)  pitch = 4.0f;

    const SfxDef* d = &SFX[id];
    for (int i = 0; i < d->n; i++) vg_synth_layer(&d->layers[i], pitch);
}

void vg_sfx_engine(bool on, float throttle) { vg_synth_engine(on, throttle); }
// The static is fired on the EDGE, here rather than in the synth, because it is a
// cue and cues live in this file. The tone that follows is held, and the two
// together are one event: the signal breaking up, and then what is left.
// File scope rather than function scope so that a silence can clear it. Left
// inside the function, a cut taken while the tone was sounding would leave the
// edge latched, and the next death would ramp up a flatline with no static in
// front of it.
static bool s_flat_was = false;

void vg_sfx_flatline(bool on) {
    if (on && !s_flat_was) vg_sfx_play(SFX_DEATH_STATIC, 1.0f);
    s_flat_was = on;
    vg_synth_flatline(on);
}

void vg_sfx_silence(void) {
    s_flat_was = false;
    vg_synth_silence();
}

void vg_sfx_update(float dt) {
    // Simulated time while a replay renders, wall time otherwise. See vg_sfx.h.
    int n = (vg_replay_mode() == VG_RP_PLAY)
          ? (int)(dt * (float)VG_AUDIO_RATE + 0.5f)
          : vg_audio_due();
    if (n <= 0) return;
    if (n > 512) n = 512;               // a frame's worth is ~370; cap the burst

    static int16_t buf[512];

    // The player's setting, squared: a linear volume slider spends most of its
    // travel doing very little, because loudness is not linear and a slider that
    // behaves as if it were feels broken at the bottom.
    // Recorded at FULL level, not at the player's setting. A capture is the game
    // as it sounds, and baking somebody's volume slider into a recording is the
    // kind of thing nobody notices until the file is the only copy left.
    {
        extern uint32_t g_sfx_render_us;
        const uint32_t t0 = micros();
        vg_synth_render(buf, n, 1.0f);
        g_sfx_render_us = micros() - t0;
    }
    vg_capture_audio(buf, n);

    if (vg.vol_sfx < 0.999f) {
        const float mix = vg.vol_sfx * vg.vol_sfx;
        for (int i = 0; i < n; i++) buf[i] = (int16_t)((float)buf[i] * mix);
    }
    vg_audio_write(buf, n);
}
