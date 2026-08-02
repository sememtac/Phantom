#pragma once
#include "vg_game.h"
#include "vg_voice.h"

// Internal contract between the simulation modules. Not part of the public API
// in vg_game.h, which is what the renderer and main loop see.
//
//   vg_models.cpp   RNG, geometry primitives, procedural models, field building
//   vg_missile.cpp  seeker guidance, proximity fuse, detonation
//   vg_ai.cpp       enemy fighter behaviour
//   vg_vfx.cpp      the host-driven explosion bench
//   vg_game.cpp     state machine, world step, player weapons, collisions

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

// --- vg_game.cpp (shared with the modules above) ---------------------------

void vg_spawn_debris(Vec3 at, float radius, int count);

// Debris with the speed and the lifetime turned up: a hull letting go rather
// than a scrape. `speed_k` and `life_k` multiply the defaults. `radius` scales
// how long each shard is; `out` is how far from the centre they launch, and
// wants to be OUTSIDE the fireball or they are never seen leaving it.
void vg_spawn_shrapnel(Vec3 at, float radius, float out, int count,
                       float speed_k, float life_k);

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

// The per-frame transform of everything: one counter-rotation and one forward
// translation applied to the arena, the backdrop and every object in the world.
// `roll_in` is radians about the view axis for this frame and goes INTO the
// rotation, so it composes with pitch and yaw instead of merely turning the
// finished picture. Flight passes zero.
void vg_world_step(float dt, float pitch_in, float yaw_in, float roll_in,
                   float throttle_in);

// The idle camera: a long lazy arc that holds the centreline of the tube.
void vg_attract_autopilot(float t, float* pitch_in, float* yaw_in);

// Put the bracket's current opponent into the world.
void vg_spawn_opponent(void);

// Put one of this pilot's lines on the radio. Higher-priority events displace
// lower ones and never the other way round, so a death is always heard out.
void vg_comms_say(const Ship* s, VoiceEvent ev);
// The broadcast voice: white, its own slot, silent during a fight.
// `badge` puts the IFT mark on the line. Off for anything that is ABOUT somebody
// else -- a fighter's name and class read as a label on that fighter, and a
// callsign block in front of it looks like whose fighter it is.
void vg_ift_say(const char* line, float hold, bool badge = true);
// Add a line to the end of the broadcast's queue, to be read after whatever is
// already up. The text is copied, so a caller's scratch buffer is safe to reuse.
void vg_ift_queue(const char* line, float hold);
// True while the broadcast is still saying something, including the silences
// inside a multi-line announcement.
bool vg_ift_busy(void);

// Lines of the current queue already begun, for pacing decisions.
int  vg_ift_progress(void);
void vg_damage_player(float amount);
// Any collision. Always fatal, and not subject to the post-hit grace period.
void vg_kill_player(void);
bool vg_player_was_hit(void);
void vg_clear_player_hit(void);

// --- vg_missile.cpp --------------------------------------------------------

// `spec` is the LAUNCHING ship's class: the round carries its warhead, seeker
// agility and burn time with it, so a BALLISTA missile behaves like a BALLISTA
// missile no matter who it is chasing.
// False if the rack is full and nothing could be launched -- the caller must not
// charge a round for a missile that was never created.
bool vg_launch_missile(bool from_player, Vec3 pos, Vec3 dir, int target,
                       const ShipSpec* spec);
void vg_update_missiles(float dt);

// --- vg_ai.cpp -------------------------------------------------------------

void vg_update_enemy(Ship* s, int index, float dt);
