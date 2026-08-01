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
#define MOTE_STREAK_SEC      0.085f   // streak length, in seconds of travel
// Streaks grow SUPER-linearly with speed. Linear growth read as slightly longer
// dashes; squaring smears them into warp lines at full throttle.
#define MOTE_STREAK_BOOST    1.35f
#define MOTE_THICK_AT        0.55f    // speed fraction above which streaks thicken
#define MOTE_FADE_IN         0.18f    // speed fraction below which they hide

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
