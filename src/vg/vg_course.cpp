#include "vg_course.h"
#include "vg_sim.h"
#include "vg_arena.h"
#include "vg_draw.h"
#include "vg_ift.h"
#include <math.h>

// Signed distance from the player to the gate's plane. The player is the origin,
// so this is just how far the plane has to travel to reach them.
//
// The sign is the whole mechanism. It starts negative with the gate ahead, goes
// positive once the plane has passed, and the frame the sign flips is the frame
// the player went through -- or did not. Nothing here cares which face they
// approached from, which is what makes a gate two-sided for free.
static inline float plane_d(void) {
    return -vdot(vg.ring_pos, vg.ring_norm);
}

static void place_ring(void) {
    // Ahead, off to one side. Not straight ahead: a gate the player is already
    // pointed at teaches nothing, and the whole exercise is putting the ship
    // somewhere it is not currently going.
    Vec3 dir = vnorm(v3(vg_frand(-0.62f, 0.62f), vg_frand(-0.52f, 0.52f), 1.0f));
    Vec3 pos = vmul(dir, vg_frand(COURSE_DIST_MIN, COURSE_DIST_MAX));

    vg.course_index++;

    // Every fourth gate hugs the boundary. This is the only place in the game
    // where the player can meet the wall alert and the red tint for free, so the
    // course puts them there on purpose rather than hoping they wander.
    const float margin = ((vg.course_index % COURSE_NEAR_WALL) == 0)
                       ? (ARENA_ALERT_RANGE * 0.55f)
                       : ARENA_SPAWN_MARGIN;
    pos = vg_arena_clamp_inside(pos, margin);

    vg.ring_pos  = pos;
    // Facing back down the approach, so the gate presents as a circle rather than
    // an edge-on line when it first appears.
    vg.ring_norm = vnorm(pos);
    vg.ring_alive = true;
    vg.ring_prev_d = plane_d();
}

void vg_course_begin(void) {
    vg.course_hits  = 0;
    vg.course_index = 0;
    vg.course_done  = false;
    vg.course_end_t = 0.0f;
    vg.health       = vg.health_max;

    // NO GATE YET. It used to be placed here, in front of a player who was still
    // being talked to -- and flying through it mid-briefing posted a score line,
    // which cuts the announcement off at the knees because an immediate line
    // clears the queue by design. The player lost most of the opening for the
    // crime of flying forwards.
    //
    // It also read as being tested before being told what the test was.
    vg.ring_alive      = false;
    vg.course_briefing = true;
    vg.course_wait     = COURSE_SETTLE;

    // NOT YET. The set is still striking, the panel is still coming up and the
    // player has just been dropped into a seat -- three things arriving at once
    // with the broadcast talking over all of them. It waits; see COURSE_GREET.
    vg.course_greet = COURSE_GREET;
}

void vg_course_reset_streak(void) {
    vg.course_hits = 0;
    // Crashing during the briefing must not conjure the gate early -- that is the
    // whole thing this was changed to avoid, and the wall is reachable from the
    // moment the course starts.
    if (vg.course_briefing) { vg.ring_alive = false; return; }
    place_ring();
}

void vg_course_update(float dt) {
    // The briefing, once the arriving has finished happening.
    if (vg.course_greet > 0.0f) {
        vg.course_greet -= dt;
        if (vg.course_greet <= 0.0f) vg_ift_line(IFT_COURSE_START);
    }

    // Finished. No more gates -- without this the run's last frame sets
    // ring_alive false and the very next one puts a fresh gate up, in the middle
    // of the beat that is supposed to be the end of it.
    if (vg.course_done) return;

    if (!vg.ring_alive) {
        // Wait out the briefing, then a quiet spell to look around in, and only
        // then start asking for something. Gates after the first are placed
        // immediately -- a pass or a miss also posts a line, and holding the next
        // gate for that would put six seconds of nothing between every attempt.
        if (vg.course_briefing) {
            // The greeting has not started yet counts as still talking. Without
            // this the settle timer runs during the pause BEFORE the briefing,
            // expires, and puts a gate up exactly as the broadcast begins --
            // which is the thing the briefing gate exists to prevent, arriving
            // by a different door.
            if (vg_ift_busy() || vg.course_greet > 0.0f) {
                vg.course_wait = COURSE_SETTLE;
                return;
            }
            vg.course_wait -= dt;
            if (vg.course_wait > 0.0f) return;
            vg.course_briefing = false;
        }
        place_ring();
        return;
    }

    const float d = plane_d();

    // Turned away and left it behind. Re-placed rather than failed: flying off is
    // not a miss, it is not attempting, and a course that punishes a change of
    // mind teaches the wrong lesson.
    if (vlen2(vg.ring_pos) > COURSE_LOST_DIST * COURSE_LOST_DIST) {
        place_ring();
        return;
    }

    if (vg.ring_prev_d < 0.0f && d >= 0.0f) {
        // The plane went past this frame. How far off centre were they when it
        // did? Project the player onto the gate's plane and measure.
        const Vec3  p     = vmul(vg.ring_pos, -1.0f);       // player, gate-relative
        const Vec3  inpl  = vsub(p, vmul(vg.ring_norm, vdot(p, vg.ring_norm)));
        const float miss  = vlen(inpl);

        if (miss <= COURSE_RADIUS) {
            vg.course_hits++;
            if (vg.course_hits >= COURSE_TARGET) {
                vg.course_done = true;
                vg.ring_alive  = false;
                vg_ift_line(IFT_COURSE_DONE);
                return;
            }
            vg_ift_line(IFT_COURSE_PASS);
        } else {
            // Back to zero. Five IN A ROW is the whole test -- a running total
            // would let a player accumulate five lucky passes over a hundred
            // attempts, which measures patience rather than control.
            vg.course_hits = 0;
            vg_ift_line(IFT_COURSE_MISS);
        }
        place_ring();
        return;
    }

    vg.ring_prev_d = d;
}

// The gate, as a ring of segments in its own plane, in the IFT's white -- the
// course is the tournament's furniture, not a pilot's.
void vg_course_draw(const VgCam& cam) {
    if (!vg.ring_alive) return;

    // Any two axes perpendicular to the normal will do; there is no preferred
    // roll for a circle.
    Vec3 a = vcross(vg.ring_norm, v3(0, 1, 0));
    if (vlen2(a) < 1e-4f) a = vcross(vg.ring_norm, v3(1, 0, 0));
    a = vnorm(a);
    const Vec3 b = vnorm(vcross(vg.ring_norm, a));

    const float TAU = 6.28318531f;
    Vec3 prev = vadd(vg.ring_pos, vmul(a, COURSE_RADIUS));

    // Not antialiased: the same call the arena grid made. A gate on approach is
    // big, bright and moving, an AA span bills by the pixel, and twenty white
    // segments across a third of the screen were a measurable slice of the
    // frame's AA cost.
    vg_line_aa_mode(false);

    for (int i = 1; i <= COURSE_SEGS; i++) {
        const float t  = TAU * (float)i / (float)COURSE_SEGS;
        const Vec3  cur = vadd(vg.ring_pos,
                               vadd(vmul(a, cosf(t) * COURSE_RADIUS),
                                    vmul(b, sinf(t) * COURSE_RADIUS)));
        vg_edge(cam, prev, cur, COL_IFT);
        prev = cur;
    }
    vg_line_aa_mode(true);
}
