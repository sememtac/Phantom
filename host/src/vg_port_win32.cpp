// The desktop port: everything behind vg_port.h, for a PC.
//
// The device implementation is vg_port_co5300.cpp and this is its opposite
// number. Only one of the two is ever compiled.
//
// WHY THE INPUT IS DONE THIS WAY, because it is the whole point of the file.
//
// The mouse does not steer the ship. The mouse is a FINGER, and the finger
// steers the ship exactly as it does on the panel. vg_input.cpp is compiled here
// completely unchanged, so the virtual joystick, its sliding origin, the 8 px
// deadzone, the 115 px full-deflection range, the v*v*0.45 + v*0.55 shaping and
// the 16/sec smoothing are not reimplemented, approximated or ported -- they are
// the same code, running on synthetic contacts.
//
// That is the only way to be sure this feels like the board. Anything that
// translated a mouse into pitch and yaw directly would be a second steering
// model to keep in step with the first, and it would drift the first time either
// was tuned.
#include "vg_port.h"
#include "cfg_display.h"
#include "cfg_hud.h"        // the throttle strip and the rear patch
#include "cfg_flight.h"     // STEER_RANGE: the deflection the centre is measured against
#include "host_window.h"
#include "host_opts.h"
#include "vg_game.h"   // vg_state_flags: only a flown state captures the pointer

#include <Arduino.h>
#include <stdio.h>
#include <string.h>
#include <math.h>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <mmsystem.h>   // waveOut: the sound output, and winmm is already linked

// Counters the device port owns and the telemetry reads.
uint32_t g_panel_wedges     = 0;
uint32_t g_in_touch         = 0;
uint32_t g_in_lock          = 0;
uint32_t g_audio_blocked_us = 0;
uint32_t g_audio_short      = 0;

// ---------------------------------------------------------------------------
// Panel
// ---------------------------------------------------------------------------

static uint16_t s_panel[SCR_W * SCR_H];

bool vg_panel_init(void) {
    // The window is already open: host_main brings it up before setup() so that
    // a failure to get one is reported before the game starts talking about
    // hardware it has not got.
    memset(s_panel, 0, sizeof(s_panel));
    return true;
}

void vg_panel_push_band(int y, int h, const uint16_t* pixels) {
    if (!pixels || y < 0 || h <= 0 || y + h > SCR_H) return;
    memcpy(&s_panel[(size_t)y * SCR_W], pixels, (size_t)h * SCR_W * 2);

    // THE LAST BAND IS THE FRAME. There is no DMA to wait on, so the moment the
    // bottom band lands the picture is complete and can go to the window. This
    // is also where the frame is paced -- see host_window_present.
    if (y + h >= SCR_H) {
        host_window_pump();
        host_window_present(s_panel);
    }
}

// Nothing is ever in flight, so there is never anything to wait for.
void vg_panel_wait(void) {}

// ---------------------------------------------------------------------------
// Touch -- the mouse and the keyboard, dressed as fingers
// ---------------------------------------------------------------------------

// THE STICK IS ANCHORED TO THE MIDDLE OF THE SCREEN, and is held as an OFFSET
// from it rather than as a position on the glass.
//
// A finger knows where it is. A hand on a mouse does not, and the first version
// let the contact wander the panel: vg_input sets its origin wherever the
// contact first lands, and STEER_RECENTER slides that origin along once the drag
// passes STEER_RANGE. So neutral drifted to somewhere invisible, and the ship
// could be turning steadily while the hand felt perfectly centred.
//
// Reporting the centre plus a CLAMPED offset fixes both ends of that. The first
// contact lands exactly at the centre, so vg_input's origin is the centre; and
// the offset can never exceed the distance that would make the origin slide, so
// it stays there. Middle of the screen is level flight, for the whole session,
// and full deflection sits at a fixed distance from it in every direction.
//
// The cost is that the mouse no longer drags for ever -- it saturates. That is
// the right trade here: an aeroplane stick has a stop, and a reference you can
// return to is worth more than travel you cannot see.
//
// THE POINTER IS THE THUMB NOW, and this is the second half of that sentence
// finally being paid for.
//
// The offset used to be a RUNNING TOTAL of raw motion, which is a relative
// control wearing a positional control's clothes. It was clamped to the right
// place and it started in the right place, but it had no way of telling the hand
// where inside that range it had got to -- so holding a steady bank meant
// remembering how far you had already pushed, and reversing meant winding the
// whole distance back before anything happened. That is the clunk.
//
// STEER_MODE 0 is a POSITIONAL stick: cfg_flight.h says the ship keeps turning
// while the finger is held displaced, and that is exactly what a thumb resting
// on glass does. So the pointer's POSITION inside the picture is the thumb's
// position, read fresh every frame and never accumulated. Hold the mouse still
// off-centre and the ship holds the turn. Put it back in the middle and you are
// level. Nothing to remember and nothing to wind back.
//
// DRIVEN BY RAW COUNTS, DISPLAYED BY THE POINTER, and both halves are load
// bearing.
//
// Reading the OS pointer's position directly was tried and it is not good enough,
// for two reasons that both only show up in the hand.
//
// ACCELERATION. "Enhance pointer precision" moves the pointer further for a fast
// hand than a slow one over the same distance of desk, so a flick reaches the
// stop and a deliberate push of the same length does not. That is the exact fault
// this file's raw input note was written about, and going positional did not
// repeal it: the mapping from pointer to stick stayed honest, but the mapping
// from HAND to pointer never was.
//
// AND THE WINDOW IS TOO SMALL TO BE A GATE. Bounding the stick by the picture
// bounds the hand by the picture too, and a wrist flick crosses any window that
// fits on a desk. Full deflection at the edge of a --scale 2 window is 482 screen
// pixels, which is a flick; the travel the control actually wants is nearer three
// times that, and there is nowhere to put it. A fence wider than the window is
// not available -- it would park the pointer over another application, where the
// fire button lands on them and takes the focus with it.
//
// So the thumb is a running total of RAW counts, clamped, and the pointer is
// placed on it every frame. Raw counts cannot be accelerated, and the total is
// clamped rather than free, so meeting the stop and coming straight back off it
// costs nothing -- which was the only thing a running total ever got wrong.
// The pointer is then a readout of the stick rather than the source of it, and
// the fence and the clamp stop at the same place because one is set from the
// other.
//
// WHAT MAKES IT POSSIBLE IS THE FENCE. An absolute mapping is only worth having
// if the hand cannot leave the range that maps to something -- otherwise the
// pointer wanders into a dead region past full deflection and coming back costs
// exactly the winding this is meant to remove. host_window_set_fence stops the
// pointer at the displacement that means full deflection, so the stop under the
// hand and the stop in the flight model are the same stop.
//
// AND POINTER ACCELERATION STOPS MATTERING, which is what raw input was for.
// Acceleration changes how far the pointer travels for a given hand movement; it
// cannot change where the pointer IS, and where it is, is the whole control now.

// The throttle thumb, in panel pixels. THROTTLE_TOP is full and THROTTLE_BOT is
// idle, and the game reads the value straight off the contact's y. Seeded to
// match vg_input's own starting throttle of 0.55 so the first frame does not
// jump.
static float s_ty = (float)THROTTLE_BOT - 0.55f * (float)(THROTTLE_BOT - THROTTLE_TOP);

// How fast the keys drive the thumb: the full strip in about 1.2 seconds, which
// is close to what a thumb does and slow enough to hold a cruise setting.
#define HOST_THROTTLE_PX_PER_SEC 235.0f

static float s_ox = 0.0f;   // offset from the centre, in panel pixels
static float s_oy = 0.0f;

// Logical pixels of stick per raw mouse count. See host_opts.h.
float g_host_stick_sens = 0.10f;

// ROLL IS SHIFT, AND NOTHING ELSE HAPPENS HERE.
//
// The board rolls by holding a button and steering: vg_input.cpp takes the
// horizontal deflection and makes it roll, with the same deadzone, shaping and
// smoothing every other control gets. Shift presses that button (see
// vg_buttons_read), so the mouse becomes the roll axis exactly as the finger
// does, and this file has nothing to add.
//
// An earlier version gave A and D their own ramped roll swipe. It worked, and it
// was wrong: it was a control the board does not have, so anything learned about
// rolling with it would not have transferred to the device. This build exists to
// mirror the board, which means declining to improve on it.

bool vg_touch_init(void) { return true; }

int vg_touch_read(uint16_t* xs, uint16_t* ys) {
    if (!xs || !ys) return 0;
    int n = 0;

    // CAPTURE THE POINTER ONLY WHILE THE SHIP IS BEING FLOWN.
    //
    // Flying wants a captured pointer that can be dragged for ever, because that
    // is what a finger on glass does. Everything else wants the opposite: the
    // cursor visible, where the player left it, and clicks that mean something.
    //
    // ASKED AS "IS THIS FLYING", NOT AS "IS THIS A MENU", and the difference is
    // not pedantic. PAUSE is deliberately not a menu -- its flags are 0, and the
    // state table says why: "a pause is not a place, it suspends one". So a
    // menu test left the pointer locked away at the exact moment the player had
    // asked for it, which is the one moment they cannot get it back.
    //
    // VGS_LIVE and VGS_ENGINE are the game's own way of saying the airframe is
    // under power and the panel is answering. That is true for PLAYING, HIT,
    // KILL and COURSE, and false for every screen, every cutscene and the pause.
    const uint8_t sf = vg_state_flags(vg.state);
    const bool flying = (sf & (VGS_LIVE | VGS_ENGINE)) != 0u;
    host_window_set_capture(flying);

    if (!flying) {
        // A TAP IS A CONTACT THAT ARRIVES, STAYS STILL AND LIFTS. vg_game.cpp
        // builds one that way: menu_edge records where it went down, every frame
        // it is held adds to a travel total, and the tap only counts if the
        // contact LIFTS having moved less than MENU_TAP_SLOP.
        //
        // So the button held is the finger down, and nothing else. An earlier
        // version reported a contact continuously so the pointer could hover,
        // and it broke selection twice over: a contact that never lifts never
        // taps, and every bit of pointer movement piled into the travel total,
        // so even a lift would have been thrown out as a drag.
        //
        // There is no hover on glass. The click position is the selection, which
        // is why none is needed here either -- and dragging still works, because
        // a contact that DOES travel is exactly what the bracket pan wants.
        // The whole picture, not the stick's box: a menu pointer has to be able
        // to reach everything that can be clicked. The thumb goes back to neutral
        // with it, so a match never begins already holding a turn.
        host_window_set_fence(0.0f);
        s_ox = s_oy = 0.0f;

        float mx = 0, my = 0;
        if (host_key_down(VK_LBUTTON) && host_window_focused()
            && host_mouse_logical(&mx, &my)) {
            xs[n] = (uint16_t)mx;
            ys[n] = (uint16_t)my;
            n++;
        }
        g_in_touch = (uint32_t)n;
        return n;
    }

    static uint32_t s_prev_us = 0;
    const uint32_t now = micros();
    float dt = s_prev_us ? (float)(now - s_prev_us) * 1e-6f : 0.0f;
    s_prev_us = now;
    if (dt < 0.0f || dt > 0.1f) dt = 0.0f;   // a stall must not fling the thumb

    // ---- the steering finger ----
    // THE THUMB IS A RUNNING TOTAL OF RAW COUNTS, held and never decayed: hold
    // the mouse still off-centre and the ship holds the turn, which is what
    // cfg_flight.h means by a positional stick.
    float mdx = 0, mdy = 0;
    host_mouse_take_delta(&mdx, &mdy);
    s_ox += mdx * g_host_stick_sens;
    s_oy += mdy * g_host_stick_sens;

    // CLAMPED BY LENGTH, not per axis, and just inside the distance that would
    // make vg_input's origin start sliding. Clamping each axis on its own would
    // allow a diagonal of 1.41x the range, which slides -- and once the origin
    // moves, the centre of the screen stops being neutral, which is the entire
    // thing this is here to prevent.
    //
    // The clamp is also what stops a running total feeling like winding. The
    // total saturates instead of running away, so the stop is a stop: coming back
    // off it moves the ship on the very next count, with no distance to undo.
    const float lim = (float)STEER_RANGE - 1.0f;
    const float len = sqrtf(s_ox * s_ox + s_oy * s_oy);
    if (len > lim) {
        const float k = lim / len;
        s_ox *= k;
        s_oy *= k;
    }

    // The pointer is a READOUT now. Fenced to the stick's own travel and placed
    // on the thumb every frame, so the hand meets the edge of the box at the same
    // moment the ship meets full deflection -- and a pointer that is hidden
    // anyway can never be somewhere the stick is not.
    host_window_set_fence(lim);

    // LIFTING THE FINGER, which is the one thing an always-engaged mouse cannot
    // do by itself. Hold this and the contact simply stops being reported: the
    // stick self-centres exactly as it does when a thumb comes off the glass.
    // C ALONE. The right button used to do this as well, and it is the roll
    // button now -- which would have meant every roll silently centring the
    // stick and dropping the lock at the moment the pilot was committing to a
    // turn.
    const bool lifted = host_key_down('C');

    // Lifting also RE-ZEROES, which is what makes it a centre key rather than
    // only a pause: the contact returns at the centre, so the origin is the
    // centre again and level flight is where it was before.
    if (lifted || !host_window_focused()) {
        s_ox = 0.0f;
        s_oy = 0.0f;
        host_mouse_centre();
    } else {
        host_mouse_place((float)(SCR_W / 2) + s_ox, (float)(SCR_H / 2) + s_oy);
        xs[n] = (uint16_t)((float)(SCR_W / 2) + s_ox);
        ys[n] = (uint16_t)((float)(SCR_H / 2) + s_oy);
        n++;
    }

    // ---- the throttle thumb ----
    // Reported only while a key is held. That is deliberate and it is what makes
    // a keyboard work here at all: with no contact in the strip the game leaves
    // the throttle exactly where it was, so tapping sets a cruise and holding
    // sweeps it -- the same behaviour as a thumb that arrives, drags and leaves.
    const bool up   = host_key_down('W') || host_key_down(VK_UP);
    const bool down = host_key_down('S') || host_key_down(VK_DOWN);
    if (up != down) {
        s_ty += (up ? -1.0f : 1.0f) * HOST_THROTTLE_PX_PER_SEC * dt;
        if (s_ty < (float)THROTTLE_TOP) s_ty = (float)THROTTLE_TOP;
        if (s_ty > (float)THROTTLE_BOT) s_ty = (float)THROTTLE_BOT;
        if (n < VG_MAX_TOUCH) {
            xs[n] = (uint16_t)(THROTTLE_ZONE_X1 / 2);   // squarely on the slider
            ys[n] = (uint16_t)s_ty;
            n++;
        }
    }

    // ---- the rear-view patch ----
    // Held like a button on the device, and the patch only claims a contact that
    // ARRIVED inside it -- which a fresh one always has.
    if (host_key_down('R') && n < VG_MAX_TOUCH) {
        xs[n] = (uint16_t)((REAR_ZONE_X0 + REAR_ZONE_X1) / 2);
        ys[n] = (uint16_t)((REAR_ZONE_Y0 + REAR_ZONE_Y1) / 2);
        n++;
    }

    g_in_touch = (uint32_t)n;
    return n;
}

uint32_t vg_i2c_denied(void) { return 0; }   // no bus, so it is never refused

// ---------------------------------------------------------------------------
// Buttons
// ---------------------------------------------------------------------------

void vg_buttons_init(void) {}

uint8_t vg_buttons_read(void) {
    uint8_t m = 0;
    // Fire is the BOOT button on the device. Space or the left mouse button
    // here: the mouse is already the aiming hand, so putting the trigger under
    // it is the arrangement a PC player expects and it changes nothing the game
    // can see.
    if (host_key_down(VK_SPACE) || host_key_down(VK_LBUTTON)) m |= VG_BTN_A;
    // The roll button, held on the right mouse button. Both hands are already
    // where they need to be -- the roll is a thing you do WITH the steering, not
    // instead of it, and reaching for a key to modify the hand that is already
    // flying is the one arrangement that cannot be done smoothly.
    //
    // Held, the mouse stops steering and starts rolling. That is the board's
    // control, not an approximation of it.
    if (host_key_down(VK_RBUTTON)) m |= VG_BTN_B;
    return m;
}

// ---------------------------------------------------------------------------
// PMU -- the power key, which is the menu key
// ---------------------------------------------------------------------------

bool vg_pmu_init(void) { return true; }

bool vg_pmu_pwr_pressed(void) {
    // An EDGE, like the device's interrupt register: the game asks whether the
    // key has been pressed since it last asked, not whether it is down.
    static bool prev = false;
    const bool now = host_key_down(VK_ESCAPE) || host_key_down(VK_RETURN);
    const bool hit = now && !prev;
    prev = now;
    return hit;
}

void vg_pmu_poll(void) {}
void vg_pmu_seen(uint8_t* st3) { if (st3) { st3[0] = st3[1] = st3[2] = 0; } }
void vg_pmu_dump(void) { Serial.println("pmu: none on this build"); }

// ---------------------------------------------------------------------------
// Storage -- two files beside the executable
// ---------------------------------------------------------------------------

static bool file_load(const char* path, void* data, unsigned len) {
    FILE* f = fopen(path, "rb");
    if (!f) return false;
    const size_t got = fread(data, 1, len, f);
    fclose(f);
    return got == (size_t)len;
}

static bool file_save(const char* path, const void* data, unsigned len) {
    FILE* f = fopen(path, "wb");
    if (!f) return false;
    const size_t put = fwrite(data, 1, len, f);
    fclose(f);
    return put == (size_t)len;
}

bool vg_store_init(void) { return true; }
bool vg_store_load(void* d, unsigned n)      { return file_load("phantom_save.bin", d, n); }
bool vg_store_save(const void* d, unsigned n){ return file_save("phantom_save.bin", d, n); }
bool vg_store_diag_load(void* d, unsigned n) { return file_load("phantom_diag.bin", d, n); }
bool vg_store_diag_save(const void* d, unsigned n) { return file_save("phantom_diag.bin", d, n); }

// ---------------------------------------------------------------------------
// Audio
// ---------------------------------------------------------------------------
//
// waveOut, because it is in winmm and winmm was already linked for the timer.
// WASAPI would be the modern answer and several hundred lines of one; this is a
// window for judging how the game feels, and the sound only has to arrive.
//
// THE GAME IS MONO. vg_synth_render fills n int16 samples at VG_AUDIO_RATE and
// the device port doubles each one into stereo because the codec wants pairs. A
// PC sound device is happy to be told it is mono, so nothing is doubled here.
//
// EIGHT BUFFERS OF 1024 SAMPLES is about 370 ms of slack. That is generous, and
// deliberately so: this build is paced by a sleep in the presenter rather than
// by a panel, so a frame can arrive late by a millisecond or two whenever
// Windows feels like it, and a short ring would turn every one of those into a
// click.

#define AU_BUFS 8
#define AU_CAP  1024

static HWAVEOUT s_wo = nullptr;
static WAVEHDR  s_hdr[AU_BUFS];
static int16_t  s_abuf[AU_BUFS][AU_CAP];
static bool     s_audio_ok = false;

bool vg_audio_init(void) {
    WAVEFORMATEX wf = {};
    wf.wFormatTag      = WAVE_FORMAT_PCM;
    wf.nChannels       = 1;
    wf.nSamplesPerSec  = VG_AUDIO_RATE;
    wf.wBitsPerSample  = 16;
    wf.nBlockAlign     = (WORD)(wf.nChannels * wf.wBitsPerSample / 8);
    wf.nAvgBytesPerSec = wf.nSamplesPerSec * wf.nBlockAlign;

    // CALLBACK_NULL: the buffers are polled through WHDR_DONE rather than
    // signalled. A callback would arrive on the driver's own thread, and this
    // build has exactly one thread on purpose -- see the note in Arduino.h.
    if (waveOutOpen(&s_wo, WAVE_MAPPER, &wf, 0, 0, CALLBACK_NULL) != MMSYSERR_NOERROR) {
        s_wo = nullptr;
        Serial.println("WARN: no audio device - the game will be silent");
        return false;
    }
    memset(s_hdr, 0, sizeof(s_hdr));
    s_audio_ok = true;
    return true;
}

// Non-blocking BY CONTRACT: takes what it can and reports how much, and the
// caller is expected to shrug at a short write. A port that blocked here would
// put the sound on the critical path of the frame.
int vg_audio_write(const int16_t* samples, int n) {
    if (!s_audio_ok || !samples || n <= 0) return 0;

    int done = 0;
    for (int b = 0; b < AU_BUFS && done < n; b++) {
        WAVEHDR* h = &s_hdr[b];
        // Still on the wire: leave it alone. A buffer is reusable once the
        // driver has flagged it done, and a fresh one has no flags at all.
        if (h->dwFlags & WHDR_PREPARED) {
            if (!(h->dwFlags & WHDR_DONE)) continue;
            waveOutUnprepareHeader(s_wo, h, sizeof(*h));
        }

        int take = n - done;
        if (take > AU_CAP) take = AU_CAP;
        memcpy(s_abuf[b], samples + done, (size_t)take * sizeof(int16_t));

        memset(h, 0, sizeof(*h));
        h->lpData         = (LPSTR)s_abuf[b];
        h->dwBufferLength = (DWORD)take * sizeof(int16_t);
        if (waveOutPrepareHeader(s_wo, h, sizeof(*h)) != MMSYSERR_NOERROR) break;
        if (waveOutWrite(s_wo, h, sizeof(*h)) != MMSYSERR_NOERROR) {
            waveOutUnprepareHeader(s_wo, h, sizeof(*h));
            break;
        }
        done += take;
    }

    // Counted where the device port counts it, so the telemetry's `short` figure
    // means the same thing on both: samples the synth made that nothing took.
    g_audio_short += (uint32_t)(n - done);
    return done;
}

// How many samples the output has consumed since the last call, from wall time.
// The device answers this from the codec's own sample clock; here the clock is
// the only one available, and over a frame it is close enough that the synth
// generates the right amount.
//
// CAPPED, because the first call after a stall would otherwise ask for every
// sample since the world began. A quarter of a second of catch-up is plenty and
// the rest is better dropped than rendered into a ring nobody is waiting on.
int vg_audio_due(void) {
    static uint32_t prev = 0;
    const uint32_t now = micros();
    if (!prev) { prev = now; return 0; }
    const uint32_t dt = now - prev;
    int n = (int)(((uint64_t)dt * VG_AUDIO_RATE) / 1000000ull);
    if (n <= 0) return 0;
    prev = now;
    const int cap = VG_AUDIO_RATE / 4;
    if (n > cap) n = cap;
    return n;
}

// The device lets this one wait, because on the board it runs on the audio task
// and the codec pacing the producer IS the mechanism. There is no audio task
// here, so waiting would be the frame waiting on itself.
int vg_audio_write_paced(const int16_t* samples, int n) {
    const uint32_t t0 = micros();
    const int done = vg_audio_write(samples, n);
    g_audio_blocked_us += micros() - t0;
    return done;
}

// ---------------------------------------------------------------------------
// IMU -- absent, which the game already knows how to handle
// ---------------------------------------------------------------------------

bool vg_imu_init(void) { return false; }
bool vg_imu_read(float*, float*, float*) { return false; }
