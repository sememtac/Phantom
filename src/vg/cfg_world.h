#pragma once

// ===========================================================================
// The world: arena boundary, asteroids, stars, speed motes, spawning.
// ===========================================================================

// --- spawning and culling --------------------------------------------------
#define SPAWN_Z_MIN          800.0f
#define SPAWN_Z_MAX          1400.0f
#define SPAWN_CONE           0.70f    // lateral spread as a fraction of z
#define CULL_Z_BEHIND        (-150.0f)
#define CULL_RADIUS          4200.0f  // dogfight ranges are long; cull well out

// --- asteroids -------------------------------------------------------------
// Deliberately sparse -- they are not the game any more. But stars sit at
// infinity and give no parallax whatsoever, so without some near geometry there
// is no way to perceive speed or closure. These are the speed cue, and a hazard
// to stay aware of while your attention is on the enemy.
#define ENABLE_ASTEROIDS     1
#define AST_TARGET_COUNT     10
#define MAX_ASTEROIDS        16
#define AST_R_MIN            10.0f
#define AST_R_MAX            32.0f
#define AST_VERTS            12
#define AST_EDGES            30
#define AST_FACES            20
#define NUM_MODELS           5

// RAISED FROM 64. A ship coming apart now throws 36 shards on its own, and at
// 64 a single death plus the missile that caused it very nearly emptied the pool
// -- which meant the next explosion silently got almost nothing. Nothing about
// space slows shrapnel down, so what sells a kill is the count and the spread,
// and both were being clipped by the ceiling rather than by the design.
// 160 shards is one primitive each, non-antialiased. See MAX_PRIMS.
#define MAX_DEBRIS           160

// --- fireballs -------------------------------------------------------------
// A ship death spends nine or ten of these and a plain detonation one to three,
// so thirty-two carries two overlapping kills before the pool starts dropping
// them. It drops silently, the same way debris does: a missing ball in the third
// simultaneous explosion is not worth a branch anywhere else.
#define MAX_FIREBALLS        32
// An explosion is an event: one that hangs around stops reading as a detonation
// and starts reading as a light source somebody left on. But the first pass was
// too brief to read at all -- at 0.32s the whole black-orange-white ramp went by
// in seven frames. A ship death scales this up (see the life multiplier on
// vg_spawn_blast); a missile fuse keeps it short.
// WIDE, on purpose. A narrow spread let the balls in one cluster expire close
// enough together to read as a single object switching off. Nearly a three-fold
// range means the group thins out raggedly instead, and Fireball::fall varies the
// SHAPE of each one's decay on top of this.
#define FIRE_LIFE_MIN        0.30f
#define FIRE_LIFE_MAX        0.85f

// --- starfield -------------------------------------------------------------
// Rotated but never translated, so they behave as if at infinity -- which is
// exactly why they convey no sense of speed and the motes below exist.
#define NUM_STARS            220
#define STAR_DIST            1000.0f

// --- speed motes -----------------------------------------------------------
// Near-field dust that streaks past. The only thing that actually sells velocity.
#define NUM_MOTES            160
#define MOTE_Z_MIN           450.0f
#define MOTE_Z_MAX           950.0f
#define MOTE_CONE            0.62f    // lateral spread as a fraction of z
#define MOTE_CULL_Z          (-50.0f)
// HOW THEY ARE DRAWN is in vg_draw_world.cpp, with draw_motes, which is the only
// thing that reads it. What stays here is where the field IS -- the extent, the
// cone and the cull plane -- because the world step, the cutscene and the model
// builder all place motes and all need to agree about the volume.

// --- arena -----------------------------------------------------------------
// Scaled up with the speed rebalance: at 250 units/sec the old 420-radius tube
// was crossed in under two seconds, which left no room to fight in.
// Scaled again with the 420 top speed. The governing number is the turn radius:
// at full throttle it is speed/rate = 420/1.33 = ~316 units, so a tube much
// tighter than this could not be turned around inside.
#define ARENA_SPHERE_R       4200.0f
#define ARENA_TORUS_RMAJ     4200.0f
#define ARENA_TORUS_RMIN     1100.0f  // room to manoeuvre, tight enough to read
                                      // as a corridor


// Torus structural hoops: how far ahead/behind they are drawn, and how far apart.
// THE TUBE IS NOT PERFECTLY ROUND, in a tournament. Prototype.
//
// A radial displacement of the boundary by periodic value noise in the torus's OWN
// coordinates (u around the loop, v around the tube). Indexed that way it is stable for
// free: the arena rides the world transform every frame, but its intrinsic coordinates do
// not move, so the terrain cannot swim as you fly.
//
// PERIODIC IN BOTH by construction -- the lattice wraps at NU and NV -- so the tunnel joins
// itself with no seam where u comes back round.
//
// WHAT IT COSTS: 244us of `sub`, measured over 1200 replayed frames against the same scene
// with only the amplitude changed. `rast`, `prim`, `can` and `upd` all moved by under 1%, so
// this does not touch the wire floor -- it is not more pixels, it is the same grid in different
// places. But `sub` is serial and runs before the flush, so those 244us land on the frame:
// about 1.5% of one, or a fps at 60. That is the price of the whole grid, main view and mirror,
// at one octave. A second octave roughly doubles it.
//
// RADIAL, which is what keeps the clearance solve O(1). Displacing along the tube's own
// normal does not move the nearest point on the tube AXIS, so vg_arena_clearance stays one
// atan2 and a length, with the local radius simply moved. That is exact for small amplitudes
// and drifts as the slope steepens, which is why the amplitude is a fraction of r_minor and
// not a free number.
// BOTH POWERS OF TWO, and that is not a rounded-off aesthetic choice. The wrap that makes the
// field periodic is the only integer work in the inner loop, and at a power of two it is a mask
// instead of four divisions. Measured: with floorf, it is the difference between 466us and 244us.
#define ARENA_WARP_NU        32       // lattice cells around the loop
#define ARENA_WARP_NV        8        // ...and around the tube
#define ARENA_WARP_OCTAVES   1        // 2 costs about twice as much per vertex
// A fraction of r_minor (1100), and this number is being LOOKED AT, not defended.
//
// It was 0.09 and the first round of a tournament -- 0.35 of it -- was invisible, correctly:
// 0.35 * 0.09 * 1100 is a 35 unit peak on a tube 2200 across, and value noise seldom reaches
// its peak, so the typical bump was nearer 12 units. Raised to 0.22 so that the mechanic can
// be judged by eye at all: +-242 units peak, and a first round still gets a third of that.
//
// Raised again to 0.30 -- +-330 units -- on the author's ask that the anomaly be plainly
// noticeable and cost the player something. Amplitude is the FREE lever here: the cost is per
// evaluation, not per unit, so a bigger displacement runs at exactly the 244us a small one does.
// Octaves are the expensive lever and buy fine detail, which is not what makes this legible.
//
// THE CLEARANCE SOLVE IS APPROXIMATE AT THIS AMPLITUDE, and the direction it errs matters. The
// radial estimate ignores the surface's slope, so on a steep inward face the true distance to
// the wall is up to about a fifth less than the number the game holds -- the boundary bites
// slightly EARLY, never late. Early is the safe direction: the player can never pass through a
// wall they can see. It reads as clipping a face rather than as being cheated, and going much
// past 0.30 is where that stops being true.
#define ARENA_WARP_MAX       0.30f
// How much of that the first round gets, against the last. Difficulty, by the author's ask:
// the final should be flown somewhere less forgiving than the opening.
//
// Not lower than this, though: the displacement arrives as an EVENT that the broadcast
// announces, and an announced event the player cannot see is worse than no event. 0.35 was
// invisible at round 0 and 0.60 was merely present. At 0.75 the first round already deforms
// hard enough to be a beat, and the bracket still has somewhere left to escalate to.
#define ARENA_WARP_ROUND0    0.75f
// THE COURSE, WHICH IS WHERE IT GETS LOOKED AT. 0 is the round tube the course was always
// flown in and is where this should end up; 1 is the full amplitude above, which is what makes
// the effect legible enough to have an opinion about. A separate knob from the tournament ramp
// so that turning the demonstration off cannot disturb the difficulty curve.
#define ARENA_WARP_COURSE    0.0f

// The episodic match events moved to cfg_events.h, which vg_config.h includes. They were
// here because the anomaly warps the arena and the arena is this file's subject -- but an
// event's schedule is not arena tuning, and a second event proved it: the surge touches the
// cockpit and has no business in a file about the world.
// ---------------------------------------------------------------------------


#define ARENA_HOOP_SPACING   480.0f
#define ARENA_HOOP_SPAN      5200.0f
#define ARENA_HOOP_SEGS      14       // enough that the ring reads round
#define ARENA_RAILS          8        // longitudinal lines down the tunnel

// Structure fades with distance: the wall you are about to hit reads brightly,
// the far side of the world sits faintly at the edge of perception. Depth, a
// frame of reference everywhere you look, and a proximity cue in one.
#define ARENA_FADE_FAR       7000.0f
#define ARENA_FADE_MIN       0.16f

// Clearance at which the boundary blends toward red, weighted by each segment's
// own distance -- so the wall you are closing on lights up while the far side
// stays cool, telling you WHERE the danger is rather than merely that there is
// some.
#define ARENA_DANGER_RANGE   1400.0f  // TEMP for a visual check

#define ARENA_ENEMY_MARGIN   700.0f   // AI turns inward inside this
#define ARENA_SPAWN_MARGIN   380.0f   // spawns get pushed this far off the wall
// Attract autopilot pulls back toward the centreline inside this clearance.
#define ARENA_ATTRACT_MARGIN 450.0f

// --- boundary alert --------------------------------------------------------
// How near is NEAR. The tube is 1100 units in radius, so the centreline sits
// 1100 from the wall -- and the alert used to fire at 900, which means it fired
// whenever the player was more than 200 units off centre. That is most of a
// match, and an alert that is almost always on is not an alert.
//
// These are deliberately far tighter than ARENA_DANGER_RANGE. That one stays wide
// because it drives the GRID reddening, which is a soft directional hint that
// says "the wall is over there" and costs nothing to ignore. These two interrupt,
// so they have to be earned.
//
// The cadence is ALERT_FLASH_* in cfg_hud.h, shared with the missile alert.
#define ARENA_ALERT_RANGE    420.0f   // the BOUNDARY annunciator

// The whole picture tints red as the wall closes, just after the annunciator.
//
// A subdivided patch of wall was tried here first and did not work: a finer mesh
// on one cell is still a detail on a wall the player is not looking at, and the
// player is looking at the enemy. A tint cannot be missed, because it is not
// somewhere on the screen -- it IS the screen.
//
// It is applied to the finished band, so it colours the instruments and the enemy
// too, the way a warning light floods a cockpit rather than lighting one dial.
#define ARENA_TINT_RANGE     380.0f   // tint starts here
