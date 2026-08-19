#pragma once
#include <stdint.h>

// Turns raw hardware (contacts, and optionally tilt) into game intent. Nothing
// here talks to a driver -- it sits entirely on top of vg_port.h, so it
// survives a move to another host firmware unchanged.
//
// Control scheme: left thumb on the throttle strip, right finger anywhere else
// to steer. A quick tap fires; so does any additional finger, so you can shoot
// without giving up the steering contact.

struct VgInput {
    float pitch;        // -1..1, nose down/up
    float yaw;          // -1..1, nose left/right
    float throttle;     // 0..1, from the left-edge slider

    bool  fire_btn;     // hardware fire button held
    bool  fire_edge;    // ...and its press edge, which is what launches

    bool  alt_edge;     // press edge of the second hardware button (menus)

    // Hold the + key and the steering swipe rolls instead of turning. There is
    // otherwise no way to roll at all, and roll is what makes a turn
    // unpredictable rather than merely fast.
    //
    // The SAME key still reports alt_edge, because it means two different things
    // in two different places: roll while flying, menu everywhere else. Which one
    // applies is the game's decision, not the input layer's.
    // PWR. The menu key: it is what the + key used to be, freed up so that key
    // could become the roll control.
    bool  pwr_edge;

    bool  roll_btn;
    float roll;         // -1..1, left/right. Zero unless roll_btn is held.

    // Held on the rear-view patch. A look, not a control: it changes nothing
    // about the ship, only which way the camera points.
    bool  rear_held;

    // tap_edge WAS HERE: the first frame of any new steering-side contact. It fired a
    // missile once. Firing is the BOOT button now, nothing read the field, and dead
    // input state that used to launch a weapon is worth removing rather than leaving
    // for somebody to reconnect by accident.
    bool  any_touch;

    // Menu pointer. Menus want ANY contact anywhere -- including the throttle
    // strip -- plus its position and frame delta, none of which the steering
    // fields can supply: those deliberately ignore the left edge and self-centre
    // on release.
    bool  menu_edge;    // press edge of any contact
    bool  menu_held;
    float menu_x, menu_y;
    float menu_dx, menu_dy;   // movement since last frame, 0 on the press frame

    // Steering state, for the on-screen indicator.
    bool  steering;
    float steer_ox, steer_oy;   // virtual-joystick origin
    float steer_x,  steer_y;    // current finger position

    float raw_ax, raw_ay, raw_az;   // tilt debugging (STEER_MODE 2)
};

void vg_input_init(void);

// THE INPUT PROBE. Start moves the whole update onto a core-0 task; take reads the
// freshest published sample. take returning false means the task never started and
// the caller should update inline exactly as before. PROBE: measuring whether core
// 0's fragmented idle can absorb the frame's input phase.
bool vg_input_probe_start(void);
bool vg_input_take(VgInput* out);

// Capture the current attitude as neutral. Only meaningful for STEER_MODE 2;
// harmless otherwise, so the game can call it unconditionally on each new run.
void vg_input_calibrate(void);

// PUT THE THROTTLE BACK, which nothing but a reboot used to do.
//
// The input layer owns the throttle: it is a slider position held by a thumb, retained
// between frames on purpose so lifting the thumb does not drop the ship out of the sky. The
// simulation's vg.throttle merely follows it, so setting that at match start achieved
// nothing at all -- the next frame overwrote it with the previous match's slider.
//
// The symptom was a new fight opening at whatever speed the last one ended at.
void vg_input_throttle_set(float t);

void vg_input_update(float dt, VgInput* out);
