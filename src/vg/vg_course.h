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

// Quiet between the RADIO OPENING and the broadcast's first word. Not between
// arriving and being spoken to -- that job moved.
//
// It was 2.2 s from the top of the match, and the reasoning was sound at the
// time: the transition, the panel coming up and the opening line all landed
// together, so the line was pushed out past them by hand. What made it wrong was
// the cockpit sequence arriving with its own pacing constants -- this number
// could no longer know when the panel had finished, so it opened over the
// power-on cue and every retune of the sequence moved the collision somewhere
// new.
//
// vg_cockpit.ready is the gate now, one second after that cue, and this is only the beat
// after it. Short, because the waiting has already been done.
#define COURSE_GREET       0.35f

// WHERE THE COURSE'S STATE LIVES, and it is here rather than in VgGame because the
// module that steps it is the module that should own it. Every field below was a
// member of `vg` and 48 of its 62 uses were already inside vg_course.cpp -- the
// struct was carrying a subsystem's private working set through a header every file
// in the game includes.
//
// One gate at a time, in VIEW space like every other object, so the world step
// carries it without the course needing to know how the world moves.
//
// prev_d is the signed distance from the player to the ring's plane on the PREVIOUS
// frame. A sign change is the crossing, which is what makes the gate passable from
// either side -- there is no front and no back, only the moment the plane goes by
// and whether you were inside the circle when it did.
struct RingCourse {
    bool    ring_alive;
    Vec3    ring_pos;
    Vec3    ring_norm;
    float   ring_prev_d;
    uint8_t hits;        // consecutive, 0..COURSE_TARGET
    uint8_t index;       // rings placed this run, for the wall-hugger
    bool    done;
    // The course holds its first gate back until the briefing is over. See
    // vg_course_update.
    bool    briefing;
    // The pilot has been identified: the WELCOME line is up. Skip unlocks here
    // rather than at the end of the briefing -- once the broadcast has said the
    // player's name, the check-in has happened and the rest is ceremony.
    bool    named;
    float   wait;
    float   greet;   // until the briefing starts
    // Time since the course was finished. The finish gets a beat of its own rather
    // than cutting straight out; see VG_COURSE.
    float   end_t;
};

// Read by the flight step, the HUD, the broadcast and the pause screen, so it is
// public the same way vg_arena is. Named to match its functions.
extern RingCourse vg_course;

// EVERYTHING BACK TO ZERO, and it exists because this state left VgGame.
//
// vg_game_init memsets `vg` whole, which used to cover the course for free. It no
// longer does, and vg_game_init is what begin_record calls to restart the game
// before a recording -- so without this a session would start carrying the course
// state of whatever was flown before it, and the replay would not reproduce.
void vg_course_clear(void);

void vg_course_begin(void);
// Streak back to zero and a fresh gate, without restarting the run. Used after a
// crash, which costs the streak and nothing else.
void vg_course_reset_streak(void);
void vg_course_update(float dt);
void vg_course_draw(const VgCam& cam);
