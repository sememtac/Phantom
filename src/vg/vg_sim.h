#pragma once
#include "vg_game.h"
#include "vg_voice.h"

// ===========================================================================
// WHAT IS LEFT OF THE SIMULATION'S SHARED CONTRACT
//
// This was 63 declarations over nine modules, grouped because one loop called them
// rather than because they shared anything -- VgGame's shape, one level up. The
// modules that own their declarations now hold them, and this file includes those
// headers so no translation unit had to change its includes for the move.
//
// The same arrangement vg_config.h has: an umbrella that also says where things went.
//
// THE BANNERS IN HERE WERE MORE THAN HALF WRONG, which is what the split was really
// for. Thirty of fifty-nine declarations sat under a module that did not define them:
// the handlers moved to vg_states.cpp in 41057fd, particles and comms had moved out
// earlier, and the headings stayed where they were. They were accurate when written.
// A heading nobody re-checks is a comment that decays into a lie, and this one had.
//
// Every banner below was DERIVED, by finding the definition of each symbol rather
// than by reading the heading above it. If declarations move again, re-derive them --
// tools exist in the session log for 2026-08-14.
// ===========================================================================

#include "vg_states.h"
#include "vg_weapons.h"
#include "vg_flight.h"
#include "vg_cockpit.h"
#include "vg_comms.h"


// --- vg_models.cpp ---------------------------------------------------------

// xorshift32. Fast, and statistical quality is irrelevant here.
void  vg_rng_seed(uint32_t s);
float vg_frand01(void);
float vg_frand(float a, float b);
Vec3  vg_rand_unit(void);

// Rotate `from` toward `to` by at most max_ang radians. This single primitive is
// what gives both missiles and fighters their finite agility, and therefore what
// makes evasion possible at all.
Vec3  vg_turn_toward(Vec3 from, Vec3 to, float max_ang);

void  vg_build_models(void);       // asteroid icosahedra + fighter hull winding
void  vg_build_starfield(void);
void  vg_build_motes(void);
Vec3  vg_mote_spawn(float zmin, float zmax);

// --- vg_game.cpp -----------------------------------------------------------

// The idle camera: a long lazy arc that holds the centreline of the tube.
// Put a fighter in slot `i`. Public only because vg_upd_attract's VG_BENCH load
// generator spawns a full complement, and the handlers now live in vg_states.cpp.
// `skill` is the turn-rate scale, already modulated by whatever the caller knows
// about the entrant. The rest of the pilot comes from `pilot`; pass null for the
// default character -- see vg_pilot.h.
void vg_spawn_enemy(int i, ShipClass cls, float skill, float hue,
                    const PilotSpec* pilot);

void vg_attract_autopilot(float t, float* pitch_in, float* yaw_in);

// Put the bracket's current opponent into the world.
void vg_spawn_opponent(void);

// One rock into the field, somewhere down the cone. Public because three
// different things populate the field: the lifecycle fills it at once, the
// dispatch tops it up while flying, and the menus keep it drifting.
void vg_spawn_asteroid(void);

// THE CHAMPION DIED. Hand the name to whoever did it and wipe the profile.
//
// Called once, at the transition into VG_OVER, and only while vg.champion. It
// persists immediately: the whole point is that this survives, and a player who
// pulls the power out after losing the title has still lost it.
//
// The volumes are NOT reset. They are settings, not something attained -- a new
// pilot inherits the room's mixer, not the last one's bank.
void vg_title_lost(void);

// --- vg_particles.cpp ------------------------------------------------------

void vg_spawn_debris(Vec3 at, float radius, int count);

// One fireball, opening to `radius` world units and drifting at `vel`. See
// struct Fireball. `life_k` multiplies FIRE_LIFE_MIN/MAX -- above 1 for a ship,
// 1 for a warhead.
void vg_spawn_fireball(Vec3 at, Vec3 vel, float radius, float life_k);

// A cluster: `balls` fireballs scattered inside `radius`, plus `shards` of
// debris at the same place, all scaled by `life_k`. Also raises the cockpit
// flash by how near and how big it was. Pass 0 shards when the caller has
// already spawned its own.
void vg_spawn_blast(Vec3 at, float radius, int balls, int shards, float life_k);

// Live fireballs, for the profiler.
int vg_fire_live(void);

// --- vg_vfx.cpp ------------------------------------------------------------

// Fire an explosion on demand so it can be looked at without playing to the
// moment that produces it. Driven from the host over the link, never from the
// game's own input. Runs wherever vg_world_step runs, the attract loop included.
#define VFX_PRESETS 5
void        vg_vfx_fire(int which);       // 0..VFX_PRESETS-1
const char* vg_vfx_name(int which);
int         vg_vfx_step_preset(void);     // next preset in the cycle, and advance
void        vg_vfx_auto(float seconds);   // repeat every N seconds, 0 to stop
float       vg_vfx_auto_period(void);
// Advances the repeat timer. Called from vg_world_step, and does nothing at all
// unless a host has asked for the repeat.
void        vg_vfx_tick(float dt);

// --- vg_missile.cpp --------------------------------------------------------

// `spec` is the LAUNCHING ship's class: the round carries its warhead, seeker
// agility and burn time with it, so a BALLISTA missile behaves like a BALLISTA
// missile no matter who it is chasing.
// False if the rack is full and nothing could be launched -- the caller must not
// charge a round for a missile that was never created.
// `shooter` is the enemy index that fired it, or -1 for the player. See
// Missile::shooter -- semi-active rounds need to know whose lock they are riding.
bool vg_launch_missile(bool from_player, Vec3 pos, Vec3 dir, int target, int shooter,
                       const ShipSpec* spec);
void vg_update_missiles(float dt);

// --- vg_ai.cpp -------------------------------------------------------------

void vg_update_enemy(Ship* s, int index, float dt);

// ===========================================================================
// RUNNING THE GAME WITH NOBODY WATCHING.
//
// Set on the desktop only, and false everywhere else -- the board has a panel
// and it is the point of the board.
//
// Two things change. The frame is a FIXED 1/60 rather than measured, because a
// simulation whose step depends on how busy the host was is not reproducible and
// cannot be a training environment. And the frame ends after the update: no
// render, no audio, no panel, which is where nearly all of the time goes.
//
// The result runs as fast as the machine allows and produces exactly the same
// fight every time from the same seed, which is what makes a measured matchup
// mean anything.
extern bool vg_headless;

// PIN THE FRAME TO 1/60 WITHOUT GIVING UP THE PICTURE.
//
// A headless run already does this, and it is the reason a measured fight
// reproduces -- but it returns before it draws, so it cannot answer a question
// about what the screen looks like. A shot taken from a live window carries the
// host's clock in it: vg.state_t is what the ticker, the sweep and the panel
// faults are drawn from, so two runs of the SAME binary put them in different
// places and a frame differs from itself by thousands of pixels.
//
// That is what made a rendering change unverifiable. The difference between two
// builds was buried under the difference between two runs, and the only thing
// that could be compared was the chassis, which is exactly the part a change to
// the drawing does not touch.
//
// Set by --shot, which is a testing flag and has no other business running at
// wall-clock speed. Frame N is then the same frame every time.
extern bool vg_fixed_dt;

// Debris with the speed and the lifetime turned up: a hull letting go rather
// than a scrape. `speed_k` and `life_k` multiply the defaults. `radius` scales
// how long each shard is; `out` is how far from the centre they launch, and
// wants to be OUTSIDE the fireball or they are never seen leaving it.
void vg_spawn_shrapnel(Vec3 at, float radius, float out, int count,
                       float speed_k, float life_k);

// The per-frame transform of everything: one counter-rotation and one forward
// translation applied to the arena, the backdrop and every object in the world.
// `roll_in` is radians about the view axis for this frame and goes INTO the
// rotation, so it composes with pitch and yaw instead of merely turning the
// finished picture. Flight passes zero.
void vg_world_step(float dt, float pitch_in, float yaw_in, float roll_in,
                   float throttle_in);

// The attract loop's set-up. Called through the STATES table like every other
// entry hook, and ALSO called directly from the won-screen exit, which is the
// kind of thing Phase 4's `leave` column is meant to stop.
// vg_enter_attract WAS DECLARED HERE and is now static in vg_states.cpp. It is an
// enter hook: it sets ATTRACT up and does not go there, and the one caller outside
// that file treated it as a transition -- which left the game stuck in WON after the
// tournament was won. Use vg_state_cut(VG_ATTRACT) or vg_state_go(VG_ATTRACT).
