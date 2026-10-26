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
#include <xinput.h>     // the pad's structures only: the DLL is loaded at run time
#include "vg_sim.h"     // vg_headless: a measurement run never reads the pad

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

// ---------------------------------------------------------------------------
// The Xbox 360 pad -- a second hand on the same glass
// ---------------------------------------------------------------------------
//
// THE PAD IS DRESSED AS FINGERS TOO, for the reason the mouse is: vg_input.cpp stays
// the one steering model. The left stick puts the steering thumb somewhere inside its
// disc, the triggers drag the throttle thumb the way W and S do, and the face buttons
// and shoulders press the same buttons and the same rear-view patch that the mouse,
// Space and R press. Nothing here reaches the simulation by any other road, so a
// sitting flown on the pad and one flown on the mouse are the same game, recorded the
// same way.
//
// XINPUT, LOADED AT RUN TIME. xinput1_4.dll is in every Windows since 8, and
// xinput9_1_0.dll is the fallback older systems carry. Loading it rather than linking
// xinput.lib keeps the build line exactly as it was, and a machine with neither simply
// has no pad.
//
// ONE PAD, the first one found, read once a frame. Asking about an empty slot is the
// slow XInput call, so with no pad the four slots are searched once a second, and
// with one only its own slot is read. Pulling the cable is a failed read and nothing
// worse: the pad is forgotten, the controls it was holding let go, and the search
// starts again.
//
// IGNORED UNLESS THE WINDOW HAS FOCUS, exactly as the keys are -- host_key_down asks
// the same question. And not read at all in a headless run, whose window never has
// focus anyway, because those runs are the measurements.
//
// THE PRINTED LINE IS FOR THE RECORDINGS. "input: xinput pad N connected" and
// "...disconnected" land in a sitting's log, so whoever reads the dataset later can
// tell which sittings were flown on the pad.

typedef DWORD (WINAPI *XInputGetStateFn)(DWORD, XINPUT_STATE*);

// The stick deadzones are XInput's own recommended figures for this pad, which are
// what a worn 360 stick needs to stop drifting: about 24% of travel for the left stick,
// which steers, and 27% for the right, which only moves the pointer in a menu. Past
// them the travel is rescaled to start from zero, so there is no step at the edge.
#define PAD_LSTICK_DEAD   XINPUT_GAMEPAD_LEFT_THUMB_DEADZONE    // 7849 of 32767
#define PAD_RSTICK_DEAD   XINPUT_GAMEPAD_RIGHT_THUMB_DEADZONE   // 8689 of 32767
// THE TRIGGERS ARE THE THROTTLE, the Ace Combat way: right opens it, left closes it,
// and the further the pull the faster it moves. A finger resting on a trigger is not
// a request, so the first eighth of the pull does nothing.
#define PAD_TRIG_DEAD     30          // of 255
// How fast the right stick moves the pointer in a menu, in logical pixels a second at
// full deflection. Squared, so a small push is fine enough to land on a button.
#define PAD_POINTER_PX_PER_SEC 700.0f

// THE FPS WAY BY DEFAULT: push the stick up and the nose comes up, the same as the
// mouse. The pad has been both ways round and split across both sticks the Ace Combat
// way, and the author's verdict was that Phantom "plays more like a fps/drone game
// rather than a plane game". --pad-invert is the flight-stick way, pull back to climb,
// for anybody who flies like that.
bool g_host_pad_invert = false;
// HOW SOFT THE MIDDLE OF THE STICK IS, 0..1. 0 is linear. Above it, the response is
// e*a^3 + (1-e)*a: a full push is still a full push, but the first part of the
// travel asks for much less. A pad stick has about a centimetre of throw for the whole
// turn rate, which reads as twitchy when it is linear -- the author's word was
// "really sensitive". At 0.6 a third of the travel asks for about a sixth of the turn.
float g_host_pad_expo = 0.6f;

static float pad_curve(float a) {
    float e = g_host_pad_expo;
    if (e < 0.0f) e = 0.0f;
    if (e > 1.0f) e = 1.0f;
    return a * ((1.0f - e) + e * a * a);
}

static XInputGetStateFn s_xget       = nullptr;
static int              s_pad        = -1;      // the slot in use, or -1
static uint32_t         s_pad_search = 0;       // millis() of the last search
static bool             s_pad_looked = false;
static XINPUT_GAMEPAD   s_gp         = {};      // this frame's reading; zero if none
static bool             s_pad_menu   = false;   // the frame is not flying: B is a key

static void pad_poll(void) {
    memset(&s_gp, 0, sizeof(s_gp));
    if (vg_headless) return;
    static bool tried = false;
    if (!tried) {
        tried = true;
        // FROM SYSTEM32 AND NOWHERE ELSE. A bare name is searched for in the exe's own
        // folder first, and phantom.exe is a signed download that lands in Downloads
        // beside whatever else is there -- so a DLL with this name dropped next to it
        // would be loaded and run inside our signed binary. Both XInputs ship in
        // System32 on every Windows this runs on, so nothing legitimate is lost. If
        // neither loads, the game runs with no pad, exactly as before.
        HMODULE m = LoadLibraryExA("xinput1_4.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (!m) m = LoadLibraryExA("xinput9_1_0.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (m) s_xget = (XInputGetStateFn)(void*)GetProcAddress(m, "XInputGetState");
    }
    if (!s_xget) return;

    XINPUT_STATE st;
    memset(&st, 0, sizeof(st));
    if (s_pad < 0) {
        if (s_pad_looked && millis() - s_pad_search < 1000u) return;
        s_pad_looked = true;
        s_pad_search = millis();
        for (DWORD i = 0; i < XUSER_MAX_COUNT; i++) {
            if (s_xget(i, &st) == ERROR_SUCCESS) {
                s_pad = (int)i;
                // THE LAYOUT RIDES ON THE CONNECT LINE. It changed three times during
                // one M1 session and again after it, and a recording's log is the only
                // record of how it was flown -- so a sitting says which layout it was,
                // and the training can tell them apart if the flying differs. "fps" was
                // the aim on the right stick; "fps-left" is the aim on the left. The
                // expo is printed as the curve uses it, clamped to 0..1.
                const float e = g_host_pad_expo < 0.0f ? 0.0f
                              : (g_host_pad_expo > 1.0f ? 1.0f : g_host_pad_expo);
                printf("input: xinput pad %d connected, layout fps-left, invert %d, expo %.2f\n",
                       s_pad, g_host_pad_invert ? 1 : 0, (double)e);
                fflush(stdout);
                break;
            }
        }
        if (s_pad < 0) return;
    } else if (s_xget((DWORD)s_pad, &st) != ERROR_SUCCESS) {
        printf("input: xinput pad %d disconnected\n", s_pad);
        fflush(stdout);
        s_pad = -1;
        s_pad_search = millis();
        return;
    }

    if (!host_window_focused()) return;
    s_gp = st.Gamepad;
}

static bool pad_btn(WORD b) { return (s_gp.wButtons & b) != 0; }

// A trigger past its deadzone, 0..1.
static float pad_trigger(BYTE v) {
    if (v <= PAD_TRIG_DEAD) return 0.0f;
    return (float)(v - PAD_TRIG_DEAD) / (255.0f - (float)PAD_TRIG_DEAD);
}

// A stick past its deadzone, as a direction and an amount: `t` is 0 at the edge of
// the deadzone and 1 at the rim, and (ux, uy) is the unit direction with +y UP the
// stick. Returns false inside the deadzone. A radial deadzone, so a diagonal push is
// neither favoured nor cut.
static bool pad_stick(SHORT sx, SHORT sy, int dead, float* ux, float* uy, float* t) {
    const float x = (float)sx, y = (float)sy;
    const float mag = sqrtf(x * x + y * y);
    if (mag <= (float)dead) return false;
    float k = (mag - (float)dead) / (32767.0f - (float)dead);
    if (k > 1.0f) k = 1.0f;     // a 360 stick reaches past 32767 on the diagonals
    *ux = x / mag;
    *uy = y / mag;
    *t  = k;
    return true;
}

bool vg_touch_init(void) { return true; }

int vg_touch_read(uint16_t* xs, uint16_t* ys) {
    if (!xs || !ys) return 0;
    int n = 0;

    // FIRST, and once: vg_input reads the buttons and the menu key after this in the
    // same frame, and they answer from this reading.
    pad_poll();

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
    s_pad_menu = !flying;

    // How long since the last frame, for everything here that moves at a rate.
    static uint32_t s_prev_us = 0;
    const uint32_t now = micros();
    float dt = s_prev_us ? (float)(now - s_prev_us) * 1e-6f : 0.0f;
    s_prev_us = now;
    if (dt < 0.0f || dt > 0.1f) dt = 0.0f;   // a stall must not fling the thumb

    if (!flying) {
        // THE PAD IN A MENU IS A MOUSE. The menus are taps and nothing else, so the
        // right stick moves the pointer and A is the button; a tap is then built from
        // it exactly as from a click. The pointer is put where the stick says and read
        // back as the click position, so the mouse and the pad can share it.
        //
        // A sub-pixel remainder is carried, or a gentle push would never move at all.
        static float s_menu_x = -1.0f, s_menu_y = -1.0f;
        float ux, uy, t;
        if (pad_stick(s_gp.sThumbRX, s_gp.sThumbRY, PAD_RSTICK_DEAD, &ux, &uy, &t)) {
            float px, py;
            if (!host_mouse_logical(&px, &py)) { px = SCR_W * 0.5f; py = SCR_H * 0.5f; }
            // Follow the real pointer unless it is still where the stick left it,
            // which keeps the fraction and lets the mouse take over at any moment.
            if (s_menu_x < 0.0f || fabsf(px - s_menu_x) > 1.5f || fabsf(py - s_menu_y) > 1.5f) {
                s_menu_x = px; s_menu_y = py;
            }
            const float v = PAD_POINTER_PX_PER_SEC * t * t * dt;
            s_menu_x += ux * v;
            s_menu_y -= uy * v;     // stick up is pointer up
            if (s_menu_x < 0.0f) s_menu_x = 0.0f;
            if (s_menu_y < 0.0f) s_menu_y = 0.0f;
            if (s_menu_x > SCR_W - 1.0f) s_menu_x = SCR_W - 1.0f;
            if (s_menu_y > SCR_H - 1.0f) s_menu_y = SCR_H - 1.0f;
            host_mouse_place(s_menu_x, s_menu_y);
        }

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
        if ((host_key_down(VK_LBUTTON) || pad_btn(XINPUT_GAMEPAD_A))
            && host_window_focused() && host_mouse_logical(&mx, &my)) {
            xs[n] = (uint16_t)mx;
            ys[n] = (uint16_t)my;
            n++;
        }
        g_in_touch = (uint32_t)n;
        return n;
    }

    // ---- the steering finger ----
    // THE THUMB IS A RUNNING TOTAL OF RAW COUNTS, held and never decayed: hold
    // the mouse still off-centre and the ship holds the turn, which is what
    // cfg_flight.h means by a positional stick.
    float mdx = 0, mdy = 0;
    host_mouse_take_delta(&mdx, &mdy);
    s_ox += mdx * g_host_stick_sens;
    s_oy += mdy * g_host_stick_sens;

    // THE LEFT STICK IS THE THUMB'S POSITION, both axes, and it wins while it is pushed.
    //
    // ONE STICK FOR THE WHOLE AIM, the way a shooter or a drone is flown: up is nose
    // up, sideways is the turn. Splitting pitch and turn across the two sticks the Ace
    // Combat way was tried and the author found it "too confusing" -- the aim is one
    // thing, so it belongs under one thumb.
    //
    // THE LEFT ONE, because the right thumb is the trigger finger. B fires, and with
    // the aim on the right stick, firing meant letting go of the aim at the moment it
    // mattered most -- in the author's words, "really hard to steer and fire missile
    // when i have to take my thumb off the stick". The right stick does nothing in
    // flight; in a menu it is still the pointer.
    //
    // A stick is positional by nature and springs back, which is exactly the thumb on
    // glass that STEER_MODE 0 models -- so its deflection is the thumb's offset from
    // the centre, read fresh every frame. When it springs back the thumb is centred
    // once, and from there the mouse can move it again: the two share one thumb.
    //
    // A ROUND DEADZONE AND A ROUND CURVE, taken on how far the stick is pushed and
    // not on each axis. Per axis, a push that is nearly but not quite sideways would
    // lose its small pitch to the deadzone and the curve, and fine corrections on a
    // diagonal would be dead. The direction is kept exactly; only the distance is
    // shaped.
    //
    // STARTING AT THE EDGE OF THE GAME'S OWN DEADZONE. vg_input already has an 8 px
    // deadzone and its v*v*0.45 + v*0.55 curve, and those are the board's feel. So the
    // first movement past the stick's deadzone lands just past the game's, and mapping
    // the rim to 0 px would have stacked a second dead band on the first.
    //
    // TWO CURVES, ON PURPOSE. pad_curve (--pad-expo) shapes the stick first, and then
    // vg_input's curve runs on top of it, the same as for a finger. The pad's own curve
    // is there because a centimetre of stick throw is far less travel than a thumb on
    // glass, and with only the game's curve the middle of the stick read as twitchy.
    // --pad-expo 0 takes the pad's curve out and leaves the game's alone.
    //
    // PITCH, BY DEFAULT, THE FPS WAY: push up and the nose comes up, the same as the
    // mouse. The flight-stick way -- pull back to climb -- is --pad-invert. See
    // g_host_pad_invert.
    //
    // A finger dragged UP the glass is nose up, so up the stick is -y on the panel.
    static bool s_pad_steer = false;
    const float pad_lim = (float)STEER_RANGE - 1.0f;
    {
        float ux = 0.0f, uy = 0.0f, t = 0.0f;
        const bool stick = pad_stick(s_gp.sThumbLX, s_gp.sThumbLY, PAD_LSTICK_DEAD,
                                     &ux, &uy, &t);
        // ROLL IS THE SHOULDERS. The board rolls by holding a button and pushing the
        // thumb sideways, so a shoulder presses that button (see vg_buttons_read) and
        // puts the thumb at the side stop -- the board's own control with the thumb at
        // full deflection. Pitch stays on its stick, because a roll you cannot pull
        // through is only a rotation of the view; see vg_input.cpp. Both together
        // cancel.
        const int roll = (pad_btn(XINPUT_GAMEPAD_RIGHT_SHOULDER) ? 1 : 0)
                       - (pad_btn(XINPUT_GAMEPAD_LEFT_SHOULDER)  ? 1 : 0);
        if (stick || roll != 0) {
            // How far, through the curve, from the edge of the game's deadzone to the
            // stop; then in the stick's own direction.
            const float r  = stick ? (float)STEER_DEADZONE
                                   + (pad_lim - (float)STEER_DEADZONE) * pad_curve(t)
                                   : 0.0f;
            const float px = ux * r;
            const float py = uy * r;
            s_ox = (roll != 0) ? (float)roll * pad_lim : px;
            s_oy = py * (g_host_pad_invert ? 1.0f : -1.0f);
            s_pad_steer = true;
        } else if (s_pad_steer) {
            s_ox = s_oy = 0.0f;
            s_pad_steer = false;
        }
    }

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
    //
    // THE PAD'S TRIGGERS ARE W AND S WITH A DIAL ON THEM: right opens the throttle,
    // left closes it. A trigger springs back, so it cannot be a throttle position --
    // letting go would cut the engine. It drives the thumb at a rate instead, the
    // keys' rate at a full pull and proportionally less below it, and released leaves
    // the throttle where it is. Both pulled together cancel.
    const bool up   = host_key_down('W') || host_key_down(VK_UP);
    const bool down = host_key_down('S') || host_key_down(VK_DOWN);
    float rate = (up ? 1.0f : 0.0f) - (down ? 1.0f : 0.0f)
               + pad_trigger(s_gp.bRightTrigger) - pad_trigger(s_gp.bLeftTrigger);
    if (rate >  1.0f) rate =  1.0f;
    if (rate < -1.0f) rate = -1.0f;
    if (rate != 0.0f) {
        s_ty -= rate * HOST_THROTTLE_PX_PER_SEC * dt;
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
    // The pad's Y is the same held contact.
    if ((host_key_down('R') || pad_btn(XINPUT_GAMEPAD_Y)) && n < VG_MAX_TOUCH) {
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
    // On the pad it is B, the Ace Combat missile button -- in flight only, because B
    // in a menu is "back" (see vg_pmu_pwr_pressed).
    if (host_key_down(VK_SPACE) || host_key_down(VK_LBUTTON)
        || (!s_pad_menu && pad_btn(XINPUT_GAMEPAD_B))) m |= VG_BTN_A;
    // The roll button, held on the right mouse button. Both hands are already
    // where they need to be -- the roll is a thing you do WITH the steering, not
    // instead of it, and reaching for a key to modify the hand that is already
    // flying is the one arrangement that cannot be done smoothly.
    //
    // Held, the mouse stops steering and starts rolling. That is the board's
    // control, not an approximation of it.
    if (host_key_down(VK_RBUTTON)) m |= VG_BTN_B;
    // A shoulder on the pad, which also puts the thumb at the side stop -- see the
    // roll note in vg_touch_read. Both together cancel there, so they cancel here.
    if (pad_btn(XINPUT_GAMEPAD_LEFT_SHOULDER) != pad_btn(XINPUT_GAMEPAD_RIGHT_SHOULDER))
        m |= VG_BTN_B;
    return m;
}

// ---------------------------------------------------------------------------
// PMU -- the power key, which is the menu key
// ---------------------------------------------------------------------------

bool vg_pmu_init(void) { return true; }

bool vg_pmu_pwr_pressed(void) {
    // An EDGE, like the device's interrupt register: the game asks whether the
    // key has been pressed since it last asked, not whether it is down.
    // On the pad: Start, anywhere. And B as well while the frame is not flying, as
    // "back one step" in a menu -- but not in flight, where one stray press would
    // pause the fight.
    //
    // B IS ALSO THE MISSILE BUTTON, so a B that is still held when the fight ends must
    // not arrive in the menu as "back". A B held in flight is blocked until it is let
    // go, and only a fresh press in the menu counts.
    static bool prev = false;
    static bool b_block = false;
    if (!s_pad_menu)                         b_block = pad_btn(XINPUT_GAMEPAD_B);
    else if (!pad_btn(XINPUT_GAMEPAD_B))     b_block = false;
    const bool now = host_key_down(VK_ESCAPE) || host_key_down(VK_RETURN)
                  || pad_btn(XINPUT_GAMEPAD_START)
                  || (s_pad_menu && !b_block && pad_btn(XINPUT_GAMEPAD_B));
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
// HOW DEEP THE QUEUE IS KEPT, in samples: 1536 is 70 ms at 22050. This is
// latency, bought not free -- every sound arrives this long after its frame --
// but it is what lets the audio survive a dropped frame without a click.
#define AU_TARGET 4096

static int audio_queued(void) {
    int q = 0;
    for (int b = 0; b < AU_BUFS; b++) {
        const WAVEHDR* h = &s_hdr[b];
        if ((h->dwFlags & WHDR_PREPARED) && !(h->dwFlags & WHDR_DONE))
            q += (int)(h->dwBufferLength / sizeof(int16_t));
    }
    return q;
}

// Ask for the DEFICIT against a target depth, not the elapsed time. The elapsed
// version handed the driver exactly what it had just played, so the ring stayed
// one frame deep and a single late frame emptied it. A deficit refills after a
// hitch; in the steady state the two agree.
int vg_audio_due(void) {
    if (!s_audio_ok) return 0;
    const int q = audio_queued();

    // AN EMPTY QUEUE IS A DROPOUT, AND NOTHING ELSE REPORTS IT. `blocked` and
    // `short` on the telemetry line only see the ring too FULL; a dry queue
    // refuses nothing and waits for nobody. Silent while healthy.
    {
        static uint32_t t0 = 0;
        static int dry = 0, frames = 0;
        const uint32_t now = micros();
        if (!t0) t0 = now;
        frames++;
        if (q == 0) dry++;
        if (now - t0 > 2000000u) {
            if (dry) Serial.printf("        AUDIO RAN DRY on %d of %d frames"
                                   " -- the sound broke up\n", dry, frames);
            dry = 0; frames = 0; t0 = now;
        }
    }
    return (q < AU_TARGET) ? (AU_TARGET - q) : 0;
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
