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
