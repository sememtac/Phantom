#pragma once
#include "vg_ship.h"

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
