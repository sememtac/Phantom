#pragma once
#include "vg_game.h"
#include "vg_voice.h"

// Internal contract between the simulation modules. Not part of the public API
// in vg_game.h, which is what the renderer and main loop see.
//
//   vg_models.cpp   RNG, geometry primitives, procedural models, field building
//   vg_missile.cpp  seeker guidance, proximity fuse, detonation
//   vg_ai.cpp       enemy fighter behaviour
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

// Put one of this pilot's lines on the radio. Higher-priority events displace
// lower ones and never the other way round, so a death is always heard out.
void vg_comms_say(const Ship* s, VoiceEvent ev);
void vg_damage_player(float amount);
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
