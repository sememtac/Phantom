#pragma once
#include "vg_proj.h"

// ===========================================================================
// THE RING COURSE
//
// A motor test. Fly through five rings in a row and the course is finished;
// miss one and the count goes back to zero. The rings are generated ahead of the
// player and can be taken from either side -- there is no correct approach, only
// the question of whether you can put the ship where you meant to.
//
// It is ALWAYS OPTIONAL. Reached from a button on the tournament map, left at any
// time, and never required to enter a round. It exists for a player who wants to
// learn the controls, and doubles as a demonstration of the game before anything
// is at stake.
//
// Dying here costs nothing. The wall is still lethal -- that is one of the things
// worth learning -- but a crash resets the count and puts you back in the tube
// with a full hull, rather than ending a tournament that has not started.
//
// One ring in five is placed deliberately close to the boundary, so the player
// meets the boundary alert and the red screen tint here, where they are free,
// rather than for the first time in a round that counts.
// ===========================================================================

#define COURSE_TARGET      5        // consecutive rings to finish
#define COURSE_RADIUS      110.0f   // gate radius, against a 9-unit ship
#define COURSE_SEGS        20       // segments in the drawn circle
#define COURSE_DIST_MIN    700.0f   // how far ahead a ring appears
#define COURSE_DIST_MAX    1150.0f
#define COURSE_LOST_DIST   2600.0f  // re-place a ring the player turned away from
#define COURSE_NEAR_WALL   4        // every Nth ring hugs the boundary
// Quiet after the briefing, before the first gate appears. Long enough to look
// around and get a feel for the controls with nothing being asked yet -- the
// course is a motor test, and the first thing to learn is what the stick does.
#define COURSE_SETTLE      2.5f

void vg_course_begin(void);
// Streak back to zero and a fresh gate, without restarting the run. Used after a
// crash, which costs the streak and nothing else.
void vg_course_reset_streak(void);
void vg_course_update(float dt);
void vg_course_draw(const VgCam& cam);
