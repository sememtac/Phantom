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
#include "cfg_hud.h"        // the throttle strip and rear patch, in panel pixels
#include "host_window.h"
#include "vg_game.h"   // vg_state_is_menu: a menu wants a pointer, not a stick

#include <Arduino.h>
#include <stdio.h>
#include <string.h>
#include <math.h>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

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

// The steering finger. Starts in the middle of the glass, which is clear of both
// the throttle strip down the left edge and the rear-view patch.
static float s_fx = (float)(SCR_W / 2);
static float s_fy = (float)(SCR_H / 2);

// The throttle thumb, in panel pixels. THROTTLE_TOP is full and THROTTLE_BOT is
// idle, and the game reads the value straight off the contact's y. Seeded to
// match vg_input's own starting throttle of 0.55 so the first frame does not
// jump.
static float s_ty = (float)THROTTLE_BOT - 0.55f * (float)(THROTTLE_BOT - THROTTLE_TOP);

// How fast the keys drive the thumb: the full strip in about 1.2 seconds, which
// is close to what a thumb does and slow enough to hold a cruise setting.
#define HOST_THROTTLE_PX_PER_SEC 235.0f

// Mouse pixels to panel pixels. 1:1 on purpose -- 115 px of hand movement is
// full deflection here exactly as it is on the glass, so anything learned about
// how hard to pull transfers to the device.
#define HOST_MOUSE_GAIN 1.0f

bool vg_touch_init(void) { return true; }

int vg_touch_read(uint16_t* xs, uint16_t* ys) {
    if (!xs || !ys) return 0;
    int n = 0;

    // A MENU IS NOT FLOWN, IT IS POINTED AT.
    //
    // Flying wants a captured pointer that can be dragged for ever, because that
    // is what a finger on glass does. A menu wants the opposite: the cursor
    // visible, where the player left it, and a click that means something. The
    // game already tells us which it is in, so the port asks rather than guesses.
    const bool menu = vg_state_is_menu(vg.state);
    host_window_set_capture(!menu);

    if (menu) {
        float mx = 0, my = 0;
        const bool inside = host_mouse_logical(&mx, &my);

        // A TAP IS A LIFT AND A TOUCH, so that is what a click is made of. The
        // menu reads menu_edge, which fires when a contact APPEARS -- and a
        // contact that is always down never appears again. Dropping it for the
        // single frame the button goes down produces a real press edge on the
        // next one, out of the game's own semantics rather than a special case.
        static bool prev_click = false;
        static int  lift = 0;
        const bool click = host_key_down(VK_LBUTTON);
        if (click && !prev_click) lift = 1;
        prev_click = click;

        if (inside && host_window_focused()) {
            if (lift > 0) { lift--; }
            else {
                xs[n] = (uint16_t)mx;
                ys[n] = (uint16_t)my;
                n++;
            }
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
    float mdx = 0, mdy = 0;
    host_mouse_take_delta(&mdx, &mdy);
    s_fx += mdx * HOST_MOUSE_GAIN;
    s_fy += mdy * HOST_MOUSE_GAIN;

    // Held inside the steering half of the glass. Clamping is not a limit on how
    // far you can turn: past 115 px the origin slides after the finger
    // (STEER_RECENTER), so a finger parked against the edge simply holds full
    // deflection, and pulling back the other way answers at once.
    const float x_lo = (float)(THROTTLE_ZONE_X1 + 2);
    if (s_fx < x_lo)                 s_fx = x_lo;
    if (s_fx > (float)(SCR_W - 2))   s_fx = (float)(SCR_W - 2);
    if (s_fy < 2.0f)                 s_fy = 2.0f;
    if (s_fy > (float)(SCR_H - 2))   s_fy = (float)(SCR_H - 2);

    // LIFTING THE FINGER, which is the one thing an always-engaged mouse cannot
    // do by itself. Hold this and the contact simply stops being reported: the
    // stick self-centres exactly as it does when a thumb comes off the glass,
    // and letting go re-acquires with a fresh origin wherever the pointer is.
    // That is not an approximation of lifting a finger, it IS lifting a finger.
    const bool lifted = host_key_down('C') || host_key_down(VK_RBUTTON);

    if (host_window_focused() && !lifted) {
        xs[n] = (uint16_t)s_fx;
        ys[n] = (uint16_t)s_fy;
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
    if (host_key_down(VK_SHIFT))                              m |= VG_BTN_B;
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
    const bool now = host_key_down(VK_RETURN);
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
// Audio -- silent, but still clocked
// ---------------------------------------------------------------------------
//
// There is no output device here yet. The synth still runs, because it is part
// of the frame's cost and part of the simulation's timing, and switching it off
// would quietly make this build cheaper and different from the board. Samples
// are accepted and dropped; the clock below is what keeps the synth generating
// the right NUMBER of them.

bool vg_audio_init(void) { return true; }

int vg_audio_write(const int16_t*, int n) { return n; }

int vg_audio_due(void) {
    // How many samples the output would have consumed since the last call, from
    // wall time -- the same question the device answers from the codec's own
    // clock, and close enough that the synth advances at the right rate.
    static uint32_t prev = 0;
    const uint32_t now = micros();
    if (!prev) { prev = now; return 0; }
    const uint32_t dt = now - prev;
    const int n = (int)(((uint64_t)dt * VG_AUDIO_RATE) / 1000000ull);
    if (n > 0) prev = now;
    return n;
}

int vg_audio_write_paced(const int16_t*, int n) { return n; }

// ---------------------------------------------------------------------------
// IMU -- absent, which the game already knows how to handle
// ---------------------------------------------------------------------------

bool vg_imu_init(void) { return false; }
bool vg_imu_read(float*, float*, float*) { return false; }
