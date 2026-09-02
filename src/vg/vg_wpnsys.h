#pragma once
#include "vg_ship.h"
#include "cfg_combat.h"   // LOCK_SPEED_PENALTY
#include "cfg_hud.h"      // RADAR_RANGE

// ===========================================================================
// THE WEAPON RULEBOOK, WRITTEN ONCE
//
// Every rule that depends on WHICH WEAPON SYSTEM a hull carries lives here, and
// both seats call it. Nothing below knows whether it is serving the player or an
// opponent, which is the point.
//
// IT WAS WRITTEN TWICE. vg_update_lock banked the player's locks and
// enemy_update_lock banked the enemy's, in two files, with comments at each site
// saying the other one was the authority -- "the player's rule and therefore this
// one". That is the one-rulebook law held up by vigilance, and vigilance is what
// the guide circle drifting onto CHARIOT already proved does not hold. Three
// rules were duplicated: what banking costs, how many rounds leave on a press,
// and what the launch spends. A fourth would have been duplicated too.
//
// TEMPLATES, AND NOT A SHARED BASE STRUCT. Weapons and Ship both already carry
// `locked`, `lock_t`, `stacks`, `stack_t` and `rounds` under those names, so a
// template binds the two with no change to either layout and no runtime cost.
// Folding the fields into a common struct was the other option and it meant
// touching about a hundred access sites across five files to gain nothing these
// do not already give -- and a hundred mechanical edits is exactly where a
// silent miss hides. If a future seat drifts a field name it fails to compile.
//
// ADDING A SYSTEM is a case in each function below plus a row in the enum and the
// invariants. Adding a SHIP that carries an existing system is a table row and
// nothing else at all.
// ===========================================================================

// Where this ship sits between its own two speeds, 0 at idle and 1 flat out.
//
// Both seats worked this out by hand, four lines each, and it is the input to the
// one rule below that decides how much the throttle costs a shot. Two hand copies
// of a normalisation is how one of them ends up unclamped.
static inline float vg_wpn_speed_norm(const ShipSpec* sp, float speed) {
    if (!sp) return 0.0f;
    const float span = sp->speed_max - sp->speed_min;
    if (span <= 0.0f) return 0.0f;
    float sn = (speed - sp->speed_min) / span;
    if (sn < 0.0f) sn = 0.0f;
    if (sn > 1.0f) sn = 1.0f;
    return sn;
}

// How long a lock takes to earn at that speed.
//
// ACQUIRING, NOT HOLDING -- see the latch below. Going fast is what makes a shot
// hard to set up; it does not revoke one already earned, and the difference is
// the whole reason the throttle is a combat control rather than a punishment.
static inline float vg_wpn_lock_need(const ShipSpec* sp, float speed_norm) {
    return sp ? sp->lock_time * (1.0f + LOCK_SPEED_PENALTY * speed_norm) : 0.0f;
}

// One frame of holding the cone, and the latch.
//
// ONCE EARNED, A LOCK IS HELD. It used to be re-derived every frame against a
// threshold that rises with speed, so opening the throttle after locking instantly
// revoked it -- the pilot was forbidden the one control the fight is about, at the
// exact moment they had committed. The lock still drops the moment the target
// leaves the cone or the range; that is the caller's test, and it is the caller
// that differs between the two seats.
template <typename W>
inline void vg_wpn_lock_hold(W& w, float dt, float need) {
    w.lock_t += dt;
    if (w.lock_t >= need) w.locked = true;
}

// What one frame of holding a lock is worth.
//
// SL-AAM banks a lock every msl_stack_time while the contact is held, and loses
// the lot the moment it is not. Every other system has no bank and this is a
// no-op for them -- which is why the caller does not test the system first.
//
// AGAINST WHAT IS IN THE BAY, not against what the bay holds. Banking a fifth
// lock with four rounds loaded banks something that cannot be fired: the salvo is
// clamped to the rack at the trigger anyway, so the extra only ever showed as a
// filled cell that did nothing. Worse on a part-spent rack -- two rounds left and
// four cells lit is the instrument lying about what the press will do.
template <typename W>
inline void vg_wpn_bank_step(W& w, const ShipSpec* sp, float dt) {
    if (!sp || sp->wpn != WPN_SLAAM) return;
    if (!w.locked) { w.stacks = 0; w.stack_t = 0.0f; return; }

    w.stack_t += dt;
    while (w.stack_t >= sp->msl_stack_time && w.stacks < w.rounds) {
        w.stack_t -= sp->msl_stack_time;
        w.stacks++;
    }
    if (w.stacks >= w.rounds) w.stack_t = 0.0f;
    // A rack that shrank under a bank. No path does this today, and the clamp
    // costs nothing against one appearing later.
    if (w.stacks > w.rounds) w.stacks = w.rounds;
}

// Losing the contact empties the bank, on the frame it happens. Called from the
// lock update's own reject paths, where there is no dt and nothing to accumulate.
template <typename W>
inline void vg_wpn_bank_drop(W& w) {
    w.stacks  = 0;
    w.stack_t = 0.0f;
}

// How many rounds leave on ONE press.
//
// One for a class that fires singly; for SL-AAM, everything banked, clamped to
// what is actually loaded. A partial release is a real choice and not a mistake:
// two now is often worth more than four in three seconds.
template <typename W>
inline int vg_wpn_salvo(const W& w, const ShipSpec* sp) {
    if (!sp || sp->wpn != WPN_SLAAM) return 1;
    int n = w.stacks;
    if (n > w.rounds) n = w.rounds;
    return (n > 0) ? n : 0;
}

// Whether the trigger has anything to send, ignoring the rack, the cooldown and
// the lock -- those are the caller's, and they are the same in both seats already.
// This is only the SYSTEM's own condition: a stacking class with an empty bank
// has nothing to fire even holding a perfect lock.
template <typename W>
inline bool vg_wpn_has_shot(const W& w, const ShipSpec* sp) {
    return !sp || sp->wpn != WPN_SLAAM || w.stacks > 0;
}

// What the launch costs, once the rounds are away.
//
// The bank is spent whatever left the rail. AND FOR SL-AAM THE LAUNCH BREAKS THE
// LOCK, which is the load-bearing one: without it the bank is permitted but never
// rewarded -- a stack lands every msl_stack_time and there is no reason on earth
// not to spend it at once, so the optimal play is a single round on that interval
// for ever. That was reported from the cockpit as spam and is the exact opposite
// of the class. Costing the lock fixes the incentive instead of papering over it
// with a longer trigger interval: the cycle becomes acquire, hold, release,
// RE-ACQUIRE, so spending a small bank throws away the contact that earned it.
template <typename W>
inline void vg_wpn_spend(W& w, const ShipSpec* sp) {
    vg_wpn_bank_drop(w);
    if (sp && sp->wpn == WPN_SLAAM) {
        w.locked = false;
        w.lock_t = 0.0f;
    }
}

// One frame of the magazine coming back, and the two systems do it oppositely.
//
// EVERY OTHER CLASS RELOADS BY DISENGAGING. The rack refills all at once and only
// from empty, so emptying it is a commitment and the seconds afterwards are the
// price. CHARIOT dumps twelve rounds in two seconds and then has nine of nothing.
//
// AR-AAM REARMS BY STAYING IN THE FIGHT. The bay works a round at a time, and only
// while a contact is held on the radar -- inside RADAR_RANGE and in the forward
// half. Fire one, keep them in front of you, and it comes back. Turn away and
// nothing rearms at all.
//
// That inversion is the whole point of the class. AEGIS was six rounds on a short
// trigger, which reads as "a lot of missiles" and not as anything in particular --
// it was mistaken for LANCE in the cockpit. Three rounds that never stop coming
// back is a rhythm nothing else in the roster has: no burst, no dry spell, and a
// reason to keep the nose pointed that no other class has.
//
// `reload` IS PER ROUND for AR-AAM and per clip for everyone else. The system says
// which, the same way it says what a trigger press costs -- one field with one
// meaning per weapon, rather than a second field sitting dead on three rows.
//
// reload_t stays a COUNTDOWN in both, because four other places read it that way,
// the observation vector among them.
template <typename W>
inline void vg_wpn_reload_step(W& w, const ShipSpec* sp, float dt, bool contact) {
    if (!sp) return;

    if (sp->wpn == WPN_ARAAM) {
        if (w.rounds >= sp->magazine) { w.reload_t = 0.0f; return; }
        // No contact, no rearm. Not a pause -- the clock does not run at all, so
        // breaking off does not quietly bank progress toward the next round.
        if (!contact) return;
        if (w.reload_t <= 0.0f) w.reload_t = sp->reload;
        w.reload_t -= dt;
        if (w.reload_t <= 0.0f) {
            w.rounds++;
            w.reload_t = (w.rounds < sp->magazine) ? sp->reload : 0.0f;
        }
        return;
    }

    if (w.rounds > 0 || w.reload_t <= 0.0f) return;
    w.reload_t -= dt;
    if (w.reload_t <= 0.0f) {
        w.reload_t = 0.0f;
        w.rounds   = sp->magazine;
    }
}

// Is this contact far enough forward to hold the bay open?
//
// `rel` is the contact in the SHOOTER's frame, +z ahead. The forward half is
// exactly the half-ellipse the radar draws: a contact behind you is parked under
// the chord as a caret, which is a warning rather than a track.
//
// NO RANGE GATE, and that is the instrument's own rule rather than a slack one.
// draw_radar clamps a contact's normalised range at the rim and keeps drawing it,
// so something at 2000 units IS on the scope -- just pinned. Gating this at
// RADAR_RANGE looked right and was measured wrong: AEGIS fights at about 1400
// mean range, so the bay almost never opened and the class fell from top of the
// roster to 226 damage a run. What holds the bay open is FACING them.
static inline bool vg_wpn_on_radar(float rel_x, float rel_z) {
    (void)rel_x;
    return rel_z > 0.0f;
}
