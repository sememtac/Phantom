#pragma once
#include "vg_vec.h"

// ===========================================================================
// THE PLAYER'S RACK, AND THE LOCK
//
// Rounds, the reload, and the seeker lock the player has to hold to earn a
// missile. vg_weapons.cpp steps all of it and holds 22 of the 50 references, so
// it owns the state -- it simply had nowhere to declare it until this header.
//
// DELIBERATELY NOT THE WHOLE MODULE. vg_update_lock, vg_player_fire,
// vg_update_threat, vg_damage_player and the rest stay declared in vg_sim.h.
// They are reached from all over the game and moving them is include churn that
// deserves its own commit rather than riding underneath a state move -- the same
// call vg_states.h made, and for the same reason.
// ===========================================================================

struct Weapons {
    int   rounds;      // remaining in the rack
    float reload_t;
    float fire_gap;
    int   target;      // enemy index, or -1
    float lock_t;      // time held inside the nose cone
    float lock_need;   // time required at the current speed
    bool  locked;
    // HOW MANY LOCKS ARE BANKED, for a class with msl_stack_time. The trigger
    // fires this many rounds at once. Emptied the moment the lock breaks.
    int   stacks;
    float stack_t;     // progress toward the next one
};

extern Weapons vg_wpn;

// Everything to zero, for vg_game_init.
//
// THIS ONE IS NOT OPTIONAL AND THE REASON IS NARROW. Most of the rack is assigned
// outright at a match start, but `lock_need` is assigned NOWHERE -- it has only ever
// been zeroed by the memset of `vg`, and the lock meter reads it with a `> 0.01f`
// guard that treats zero as "fall back to the hull's lock time". Left out of the
// struct's reach it would carry the previous run's value into a recording, and the
// meter would be scaled against a ship that is not being flown.
void vg_wpn_clear(void);

// ---------------------------------------------------------------------------
// WHAT IS SHOOTING AT YOU
//
// The nearest live missile tracking the player, re-derived from scratch by
// vg_update_threat every frame it runs -- `on` false and `range` 1e9 at the top of
// the scan, so nothing here persists on purpose.
//
// It drives two things that are not weapons: the cockpit's missile annunciator and
// the HUD's off-screen arrow. It lives with the weapons anyway, because the scan
// that fills it is here and a reader chasing "where does the missile warning come
// from" should land in one file.
// ---------------------------------------------------------------------------

// Kept from where this lived before, because it explains the shape and not the
// location: `on` rather than a bare `threat`, because the group is the noun and the
// flag is one of its fields -- vg.threat.on said which, where vg.threat did not.
struct Threat {
    bool  on;
    float range;
    Vec3  pos;
};

extern Threat vg_threat;

// For vg_game_init, and it IS needed despite the scan rebuilding the struct.
//
// vg_update_threat only runs from vg_upd_playing. Between a begin_record restart and
// the first flying frame the game sits in ATTRACT, where nothing rewrites this and
// the HUD still reads `on` -- so a stale true would put a threat arrow over the
// attract loop of a fresh recording.
void vg_threat_clear(void);

// --- MOVED FROM vg_sim.h -------------------------------------------------
//
// The rest of the module, which this header said would come in its own commit
// rather than riding underneath a state move. This is that commit.

// One frame of the player's weapons. Called from the state dispatch, which is
// what decides whether a state is allowed to shoot at all -- these do not check.
void vg_update_lock(float dt);
void vg_update_reload(float dt);
void vg_player_fire(void);
// Nearest live enemy missile tracking the player, for the threat warning.
void vg_update_threat(void);

void vg_damage_player(float amount);
// Any collision. Always fatal, and not subject to the post-hit grace period.
void vg_kill_player(void);
bool vg_player_was_hit(void);
void vg_clear_player_hit(void);
