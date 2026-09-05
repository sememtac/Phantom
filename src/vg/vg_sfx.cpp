#include "vg_sfx.h"
#include "vg_prof.h"
#include <Arduino.h>
#include "vg_synth.h"
#include "vg_port.h"
#include "vg_game.h"
#include "vg_capture.h"
#include "vg_replay.h"

VgVolume  vg_vol;
VgDisplay vg_disp = { true };   // scanlines on, which is how the game has always looked

// Just the mixer, to tell it from the I2S write. Defined here because this is the
// file that stops the clock on it.
uint32_t g_sfx_render_us;

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
    // IT USED TO BE HELD BACK 0.45s, and that delay is gone. The reason for it was
    // that entering the course set the panel booting inside the transition's join,
    // one frame before the set struck, so without a delay both landed together and
    // neither was heard. The cockpit sequence moved this cue a second and a half
    // into the match -- the set has long since struck by the time it fires, and the
    // delay had stopped being a fix and become a lie: the radio gate is measured
    // from the moment this is PLAYED, so half a second of silent head made the
    // author's "one second after the ready sound" into 0.55 in the ear.
    //
    // The internal spacing is unchanged, only the offset.
    { SW_SQUARE,   62,    44,   0.55f,  0.006f,  0,     0.40f,  500,  0,       0,  0 },
    { SW_SQUARE,  392,   392,   0.12f,  0.004f,  0,     0.26f, 4200,  0.02f,   0,  0 },
    { SW_SQUARE,  523,   523,   0.12f,  0.004f,  0,     0.26f, 4200,  0.15f,   0,  0 },
    { SW_SQUARE,  784,   784,   0.26f,  0.004f,  0,     0.26f, 4200,  0.28f,   0,  0 },
};

static const SynthLayer L_PANEL_ON[] = {
    // ONE REGION LATCHING. Forty milliseconds, so it is a tick that happens to have
    // a pitch rather than a note -- four of these land inside a second at the
    // current rate, and anything longer runs them together into a chirp.
    //
    // Swept up a little across its own life, which is most of the character: a blip
    // that rises reads as something switching ON. Flat, it is a counter ticking.
    //
    // Quieter than the radio blip, and deliberately the quietest cue in the file.
    // It fires four times in quick succession and it is the panel talking to
    // itself, not to the player. The caller pitches each one higher than the last,
    // so the four are a rising figure rather than four of the same tick.
    { SW_SQUARE, 1240,  1480,   0.040f, 0.0012f, 0,     0.15f, 7000,  0,       0,  0 },
};

static const SynthLayer L_SPOOL[] = {
    // THE WIND-UP IN THE DARK, and the brief was "faint" -- it should be findable
    // rather than heard. It fills CANOPY_INTRO_LEAD, the second before the first
    // region lights, and it is the only thing in the game that plays to a black
    // screen.
    //
    // A slow attack because it has to arrive by SWELLING. Any transient at the
    // front would be an event, and the point of that second is that nothing has
    // happened yet.
    //
    // Swept up to 540 rather than down low where a wind-up "should" sit: the note
    // at the top of this file is that the driver reproduces very little below a few
    // hundred Hz, so a rise that stays under it is a rise nobody hears. 26 Hz of
    // tremolo is what makes it a machine instead of a tone -- past 15 it stops
    // juddering, and it has not yet fused into timbre at 30.
    { SW_SQUARE,   88,   540,   0.94f,  0.26f,   0.06f, 0.14f, 1900,  0,      26,  0.5f },
    // ...and something turning under it. Noise this quiet is not heard as noise; it
    // is heard as the tone having a mechanism.
    { SW_NOISE,     0,     0,   0.86f,  0.42f,   0,     0.05f, 1300,  0.05f,   0,  0 },
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
    CUE(L_PANEL_ON),
    CUE(L_SPOOL),
    CUE(L_IFT),
    CUE(L_IFT_SHORT),
    CUE(L_DEATH_STATIC),
};

// ---------------------------------------------------------------------------

// ===========================================================================
// THE SEAM: the synth runs on the OTHER CORE
//
// Measured cause. Mixing cost 453 us in a quiet frame and up to 4341 us in a busy
// one -- 27% of a 16.67 ms budget -- and it billed to the submit phase, in front
// of the panel flush, so it delayed the whole transfer. The variable is voices,
// not samples: 1.23 us/sample quiet against 8.48 us/sample busy, while the sample
// count can only rise 1.39x because vg_sfx_update already caps it at 512. Capping
// harder would drop ~2.6 ms holes into frames that are already late, which trades
// a frame-time problem for an audible one.
//
// So it moves off the render core instead of being shaved. CONFIG_ARDUINO_RUNNING_CORE
// is 1, so loop() and all the drawing are on core 1 and core 0 is free.
//
// WHY THIS IS SAFE, and it is not luck -- vg_synth.cpp was already built for it:
//   - it touches NO game state at all (`grep -c "vg\\." vg_synth.cpp` is zero)
//   - it has its own RNG, and says why: drawing from the game's stream "would make
//     the audio a term in the simulation and take replay determinism with it"
// So the only shared state is the synth's own voices, and this file is the only
// thing that talks to them.
//
// OWNERSHIP: the audio task owns the voices, the engine and the flatline outright.
// Nothing else touches them, so there is no lock on voice state anywhere. The game
// thread only ever posts events.
// ===========================================================================

enum SfxEvKind : uint8_t { EV_LAYER, EV_ENGINE, EV_FLATLINE, EV_SILENCE, EV_RESET };

struct SfxEv {
    uint8_t           kind;
    bool              on;
    float             a;       // pitch for a layer, throttle for the engine
    const SynthLayer* layer;   // into static const SFX[], so it outlives the queue
};

// Single producer (the game thread), single consumer (the audio task). Power of
// two so the wrap is a mask. 64 is far more than a frame ever posts -- a busy
// frame is a handful of cues -- and the overflow policy is to DROP, because a
// missed sound effect is better than a blocked renderer.
#define SFX_Q 64
static SfxEv   s_q[SFX_Q];
static uint32_t s_q_head = 0;   // written by the game thread only
static uint32_t s_q_tail = 0;   // written by the audio task only

static void q_push(uint8_t kind, bool on, float a, const SynthLayer* l) {
    const uint32_t h = s_q_head;
    // ACQUIRE on the tail: we have to see the consumer's progress before deciding
    // the ring is full, or a full-looking ring stays full forever.
    if (h - __atomic_load_n(&s_q_tail, __ATOMIC_ACQUIRE) >= SFX_Q) return;
    s_q[h & (SFX_Q - 1)] = SfxEv{ kind, on, a, l };
    // RELEASE, and last: the slot must be visible before the index that publishes
    // it, or the consumer can read an entry that has not been written yet.
    __atomic_store_n(&s_q_head, h + 1, __ATOMIC_RELEASE);
}

// Applies whatever has been posted. Called by whoever is about to render, which
// is what lets the producers stay identical in both modes.
static void q_drain(void) {
    uint32_t t = s_q_tail;
    const uint32_t h = __atomic_load_n(&s_q_head, __ATOMIC_ACQUIRE);
    while (t != h) {
        const SfxEv& e = s_q[t & (SFX_Q - 1)];
        switch (e.kind) {
        case EV_LAYER:    vg_synth_layer(e.layer, e.a);   break;
        case EV_ENGINE:   vg_synth_engine(e.on, e.a);     break;
        case EV_FLATLINE: vg_synth_flatline(e.on);        break;
        case EV_SILENCE:  vg_synth_silence();             break;
        default:          vg_synth_reset();               break;
        }
        t++;
    }
    __atomic_store_n(&s_q_tail, t, __ATOMIC_RELEASE);
}

// INLINE MODE, and both cases are mandatory rather than tuning.
//
// A REPLAY must be deterministic: vg_sfx_update already switches to simulated time
// for VG_RP_PLAY, and vg_capture_audio writes exactly those samples into the .phr.
// A task rendering on wall-clock time would put different audio in the file every
// run.
//
// A CAPTURE writes the link, and so does the band flush. Two cores writing Serial
// would interleave mid-packet and corrupt the pixel stream -- which is precisely
// the failure the per-band Adler-32 exists to diagnose, and there is no reason to
// manufacture more of it.
//
// Neither case cares about frame time, which is why this costs nothing.
// Did the mixing task actually start? Every other task in this project treats a
// failed creation as a supported outcome and falls back -- rowsplit_start says a
// failure "costs frame rate and nothing else", and vg_input drops to reading the
// IMU on the render thread. This one ignored the answer, and the fallback did not
// exist: with no task, normal play does no mixing at all and the game is silent
// for ever with nothing anywhere saying why.
static bool          s_task_ok       = false;
static volatile bool s_inline_want   = false;   // set by the game thread
static volatile bool s_task_parked   = true;    // acknowledged by the task
static int16_t       s_buf[512];

// Render whatever is due and hand it to the codec. One body, both callers.
//
// `paced` picks which write: the audio task may wait for room in the DMA ring and
// wants to, because that wait is what keeps the ring full and the producer honest.
// The render thread may not, for the reason vg_port.h gives at some length.
static void sfx_render_due(int n, bool paced) {
    if (n <= 0) return;
    if (n > 512) n = 512;               // a frame's worth is ~370; cap the burst

    {
        // Recorded at FULL level, not at the player's setting. A capture is the
        // game as it sounds, and baking somebody's volume slider into a recording
        // is the kind of thing nobody notices until the file is the only copy left.
        const uint32_t t0 = micros();
        vg_synth_render(s_buf, n, 1.0f);
        g_sfx_render_us = micros() - t0;
    }
    vg_capture_audio(s_buf, n);

    // The player's setting, squared: a linear volume slider spends most of its
    // travel doing very little, because loudness is not linear and a slider that
    // behaves as if it were feels broken at the bottom.
    if (vg_vol.sfx < 0.999f) {
        const float mix = vg_vol.sfx * vg_vol.sfx;
        for (int i = 0; i < n; i++) s_buf[i] = (int16_t)((float)s_buf[i] * mix);
    }
    if (paced) vg_audio_write_paced(s_buf, n);
    else       vg_audio_write(s_buf, n);
}

// A FIXED CHUNK, about 11.6 ms of audio, and the write is what decides when the
// next one is wanted. Big enough that the per-pass overhead disappears against the
// mixing; small enough that a cue posted just after a pass began is looked at
// promptly, since q_drain only runs between chunks.
#define SFX_CHUNK 256

static void sfx_task(void*) {
    for (;;) {
        if (s_inline_want) {
            // Parked: the game thread is about to render, or already is. Say so
            // and touch nothing -- this flag is the handshake, and a request flag
            // alone would let both sides render the same voices at once.
            s_task_parked = true;
            vTaskDelay(pdMS_TO_TICKS(4));
            continue;
        }
        s_task_parked = false;
        q_drain();
        // PACED BY THE CODEC, not by a delay and not by a clock. The write returns
        // when the ring has room for the next chunk, so this loop runs at exactly
        // the sample rate without anything here having to know what that is.
        sfx_render_due(SFX_CHUNK, true);
        // Insurance, not pacing. If the codec never came up the write returns
        // immediately, and a tight loop at priority 2 would starve core 0's idle
        // task and trip its watchdog -- a silent speaker should not panic the game.
        vTaskDelay(1);
    }
}

bool vg_sfx_init(void) {
    vg_synth_reset();
    if (!vg_audio_init()) return false;

    // CORE 0. loop() is on core 1 (CONFIG_ARDUINO_RUNNING_CORE), which is where
    // every microsecond of this was being charged. Priority 2 sits above idle and
    // below nothing that matters on this core; the task yields every pass, so it
    // cannot starve anything, and it is deliberately NOT registered with the task
    // watchdog -- it is not the loop, and a stall here should not panic the game.
    s_task_ok = (xTaskCreatePinnedToCore(sfx_task, "sfx", 4096, nullptr, 2,
                                        nullptr, 0) == pdPASS);
    return true;
}

void vg_sfx_play(SfxId id, float pitch) {
    if (id >= SFX_COUNT) return;
    if (pitch < 0.25f) pitch = 0.25f;
    if (pitch > 4.0f)  pitch = 4.0f;

    // POSTED, never applied here, in either mode. The producers do not need to
    // know which core is rendering -- whoever renders drains first -- and that is
    // what keeps one code path instead of two that have to agree.
    const SfxDef* d = &SFX[id];
    for (int i = 0; i < d->n; i++) q_push(EV_LAYER, false, pitch, &d->layers[i]);
}

void vg_sfx_engine(bool on, float throttle) {
    q_push(EV_ENGINE, on, throttle, nullptr);
}
// The static is fired on the EDGE, here rather than in the synth, because it is a
// cue and cues live in this file. The tone that follows is held, and the two
// together are one event: the signal breaking up, and then what is left.
// File scope rather than function scope so that a silence can clear it. Left
// inside the function, a cut taken while the tone was sounding would leave the
// edge latched, and the next death would ramp up a flatline with no static in
// front of it.
static bool s_flat_was = false;

void vg_sfx_flatline(bool on) {
    // The EDGE stays on this thread. It is a game decision -- somebody just died
    // -- and the queue preserves order, so the static and the tone behind it reach
    // the synth in the order they were posted.
    if (on && !s_flat_was) vg_sfx_play(SFX_DEATH_STATIC, 1.0f);
    s_flat_was = on;
    q_push(EV_FLATLINE, on, 0.0f, nullptr);
}

void vg_sfx_silence(void) {
    s_flat_was = false;
    q_push(EV_SILENCE, false, 0.0f, nullptr);
}

void vg_sfx_update(float dt) {
    // Which side is rendering this frame. Requested here and acknowledged by the
    // task, because a request on its own would let both render the same voices for
    // however long the task took to notice.
    // ...OR BECAUSE THERE IS NOBODY ELSE. A replay and a capture render inline so
    // the frame owns the sound; so does a build whose mixing task never started,
    // which would otherwise be silent and never say so.
    // ...BUT NOT A TIMED RUN. Nothing records its audio, so determinism buys it
    // nothing, and mixing inline put up to 4 ms of samples into `sub` on exactly
    // the frames the recording took slowly -- the dips the run exists to measure,
    // inflated by an artefact live play never pays. The task mixes, as it does in
    // flight, contending for core 0 as it does in flight.
    const bool want_inline = !s_task_ok
                          || (vg_replay_mode() != VG_RP_OFF && !vg_replay_timed())
                          || vg_capture_active();
    s_inline_want = want_inline;

    if (!want_inline) {
        // The task owns the audio. This is the whole point: in normal play the
        // frame does no mixing at all.
        return;
    }

    // Wait for the task to park before touching synth state. Bounded in practice
    // by the task's 4 ms pass, and it terminates immediately if the task was never
    // created -- s_task_parked starts true, so a board with no audio never waits.
    while (!s_task_parked) vTaskDelay(1);

    q_drain();

    // Simulated time while a replay renders, wall time otherwise. See vg_sfx.h.
    //
    // Note vg_audio_due() keeps a static timestamp, so the first call after a mode
    // switch sees the whole gap since the other side last asked and returns a large
    // count. It is capped, and a mode switch is the start of a capture or a replay
    // -- a click there is not a thing anybody is listening to.
    sfx_render_due((vg_replay_mode() == VG_RP_PLAY)
                   ? (int)(dt * (float)VG_AUDIO_RATE + 0.5f)
                   : vg_audio_due(),
                   false);
}
