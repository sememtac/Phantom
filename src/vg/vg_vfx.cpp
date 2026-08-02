#include "vg_sim.h"
#include "vg_replay.h"

// --- the VFX bench ---------------------------------------------------------
//
// Fire any explosion in the game on demand, from the host, without playing to
// the moment that produces it. Same reasoning as the 'z' command in
// vg_capture.cpp: the alternative was flying a whole match to look at one
// third of a second of effect, and then doing it again after every tuning
// change.
//
// It runs wherever vg_world_step runs, which includes the attract loop -- so the
// intended use is to sit on the title screen and watch, with no match involved.
// Nothing here is reachable from the game's own input; a host has to ask.
//
// Its own file because it owes the rest of the simulation nothing: three statics
// that no other module touches, and calls out only to the spawners and the RNG.
// It is a tool that happens to live in the firmware, and keeping it apart from
// the game state is what stops it acquiring any.

static int   s_vfx_next = 0;
static float s_vfx_auto = 0.0f;   // seconds between shots, 0 = off
static float s_vfx_t    = 0.0f;

static const char* const VFX_NAME[VFX_PRESETS] = {
    "missile fuse expires", "missile hit", "ship destroyed", "player wreck",
    "point blank -- inside the fire",
};

void vg_vfx_fire(int which) {
    if (which < 0 || which >= VFX_PRESETS) which = 0;
    // Scattered, so repeated shots are not the same picture twice and the size
    // can be judged against something other than the centre of the screen.
    const Vec3 at = v3(vg_frand(-46.0f, 46.0f), vg_frand(-34.0f, 34.0f),
                       vg_frand(210.0f, 300.0f));
    // Right on the canopy, so the knock bus is exercised where it actually bites:
    // inside the fireball's own radius, which is the only place the rumble and the
    // panel glitch come from. Everything else fires far enough out that those two
    // paths never run, and they were the hardest part to judge from a distance.
    const Vec3 near_at = v3(vg_frand(-14.0f, 14.0f), vg_frand(-10.0f, 10.0f),
                            vg_frand(18.0f, 30.0f));
    switch (which) {
    case 0:  vg_spawn_blast(at,  7.0f,  1, 3,  1.0f); break;
    case 1:  vg_spawn_blast(at, 16.0f,  3, 8,  1.0f); break;
    case 2:  vg_spawn_blast(at, 46.0f,  9, 0,  1.9f);
             vg_spawn_shrapnel(at, 30.0f, 54.0f, 34, 4.4f, 1.8f); break;
    case 3:  vg_spawn_blast(at, 62.0f, 11, 0,  2.2f);
             vg_spawn_shrapnel(at, 40.0f, 72.0f, 44, 4.8f, 2.0f); break;
    default: vg_spawn_blast(near_at, 46.0f, 9, 0, 1.9f);
             vg_spawn_shrapnel(near_at, 30.0f, 54.0f, 34, 4.4f, 1.8f); break;
    }
}

const char* vg_vfx_name(int which) {
    if (which < 0 || which >= VFX_PRESETS) which = 0;
    return VFX_NAME[which];
}

int vg_vfx_step_preset(void) {
    const int w = s_vfx_next;
    s_vfx_next = (s_vfx_next + 1) % VFX_PRESETS;
    return w;
}

void vg_vfx_auto(float seconds) {
    s_vfx_auto = seconds;
    s_vfx_t    = 0.0f;
}

float vg_vfx_auto_period(void) { return s_vfx_auto; }

void vg_vfx_tick(float dt) {
    if (s_vfx_auto <= 0.0f) return;
    // NOT DURING A RECORD OR A RENDER. The one-shot command is already barred in
    // vg_capture.cpp, but the repeat is a timer and would have kept firing right
    // through a session.
    //
    // It would not have looked like a determinism bug either. The simulation is a
    // pure function of seed, dt and input only while every draw from the xorshift
    // happens in both passes -- and a shot fired during the record has no reason
    // to fire during the render, because the flag is off after a reset. From that
    // frame on the two runs are drawing different numbers and the replay quietly
    // stops matching the game it recorded.
    if (vg_replay_mode() != VG_RP_OFF) return;
    s_vfx_t -= dt;
    if (s_vfx_t > 0.0f) return;
    s_vfx_t = s_vfx_auto;
    vg_vfx_fire(vg_vfx_step_preset());
}
