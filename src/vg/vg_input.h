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

    bool  tap_edge;     // first frame of any new steering-side contact
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

// Capture the current attitude as neutral. Only meaningful for STEER_MODE 2;
// harmless otherwise, so the game can call it unconditionally on each new run.
void vg_input_calibrate(void);

void vg_input_update(float dt, VgInput* out);
