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

#define MAX_DEBRIS           64

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

#define ARENA_WARN_RANGE     450.0f   // blinking warning below this

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
// A flash whose rate IS the range. One a second when the wall first becomes a
// problem, accelerating to four a second at contact and never faster, because
// past that it stops reading as a rhythm and becomes a flicker.
#define ARENA_ALERT_SLOW     1.00f    // seconds per flash at ARENA_DANGER_RANGE
#define ARENA_ALERT_FAST     0.25f    // ...and at the wall. Never faster.

// The subdivided patch of wall the player is about to hit.
//
// The structural grid is 480 units between hoops and an eighth of the tube
// between rails, which is a cell hundreds of units across. Reddening a cell that
// large says "the wall is near" without saying HOW near, and a player can cross
// the last of it without a cell boundary ever passing them. So the cell the
// player is closest to gets subdivided, and the mesh appearing and tightening is
// the cue that the surface is arriving.
#define ARENA_PATCH_SPAN     520.0f   // world units across the patch
#define ARENA_PATCH_SUB      3        // extra lines each side of the near point
#define ARENA_PATCH_SEGS     6        // segments per patch line
