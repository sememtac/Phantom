#pragma once
#include <stdint.h>
#include "vg_vec.h"
#include "vg_config.h"

// ===========================================================================
// THE PLAYER'S RIBBON
//
// The contrail behind the player's own ship. Visible because the world
// counter-rotates around a fixed camera, so a hard turn sweeps your own track
// into view -- you can see the arc you just flew. Every other ship carries its trail
// inside Ship; the player has no Ship, so theirs lived in VgGame -- which is where
// a module's state goes when the module has no header. vg_flight.cpp lays these
// points down and holds 11 of the 18 references, so it owns them.
//
// NOT trail_hue. That is identity, it is one of the five fields RpSave carries into
// a recording, and it stays in VgGame. The two only ever looked like one group
// because they share a prefix.
//
// `p` is the throttle setting each point was laid down at, 0..255 -- what makes the
// contrail lengthen under power and persist after the ship has backed off.
// ===========================================================================

struct PlayerTrail {
    float   acc;
    uint8_t n;
    uint8_t head;
    uint8_t p[SHIP_TRAIL];
    Vec3    pt[SHIP_TRAIL];
};

extern PlayerTrail vg_trail;

// Back to no ribbon. Called from vg_game_init: these fields DID rely on its memset.
// The two places that reset the trail by hand are a match start and a course start,
// and neither is on the path begin_record takes -- it restarts the game and drops
// straight into ATTRACT, so without this a recording made after flying would open
// trailing the previous run's ribbon.
void vg_trail_clear(void);

// --- MOVED FROM vg_sim.h -------------------------------------------------

// The roll command as an angle for this frame. Public because the three states
// that fly all pass it INTO vg_world_step, and it depends on the smoothed
// throttle -- roll authority should lag a shove of the slider exactly as speed
// does.
float vg_roll_angle(const VgInput* in, float dt);

// The close-aboard knock: a fighter crossing inside ten ship lengths. Separate
// from the world step because only a live match calls it -- there is nothing to
// pass in the course or the attract loop.
void vg_update_passes(void);

// ===========================================================================
// THE BOUNDARY, AS THIS AIRFRAME EXPERIENCES IT
//
// Not the arena -- that is vg_arena.h, and it is geometry that exists whether or not
// anyone is flying. This is the pair that says how THIS ship stands against it, and it
// lives with the flight model because the world step is the only thing that maintains it.
//
// ITEM 2 CALLED THIS GROUP OWNERLESS and it looked that way: six writes across five
// files. But five of them are the same line copied -- a bare reseed of the clearance on a
// state entry -- and only vg_flight.cpp MAINTAINS the pair, because only it has the
// previous clearance to take a derivative of. Seeding is not owning, the same way
// resetting was not owning for the cockpit's boot chain.
struct Wall {
    // Distance to the arena boundary, recomputed every frame of the world step.
    float clearance;
    // HOW FAST THE WALL IS ARRIVING, units a second, positive while closing.
    //
    // Distance alone cannot say "you are about to hit this". A ship holding station a
    // hundred units off the boundary is safe indefinitely; the same hundred units with the
    // nose pointed at it is a second of life. So the warning reads BOTH: the colour comes
    // from the clearance and the flashing comes from this.
    //
    // The RATE and not the speed, deliberately. Speed would cry wolf every time a fast
    // hull ran parallel to the wall, which is most of a tunnel fight, and it would say
    // nothing about a slow drift straight into it. This is d(clearance)/dt, so it is only
    // large when the clearance is actually being spent.
    //
    // Smoothed: one frame's difference of two clearances is mostly noise, because the
    // clearance comes out of a torus solve that moves with the whole world transform.
    float rate;
};

extern Wall vg_wall;

// Recompute the clearance for where the ship is NOW, leaving the rate alone.
//
// This was six verbatim copies of one expression, in five files. They are all the same
// thing -- a state entry that must not let the alarm fire off a clearance measured in the
// last venue -- and now they say so. The world step calls it too, having first kept the
// old clearance to difference against.
void vg_wall_seed(void);

// Both to zero, for vg_game_init. They relied on the memset like everything else that has
// left VgGame, and a stale clearance at boot is an alarm on the title screen.
void vg_wall_clear(void);

