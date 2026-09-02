#include "vg_sim.h"
#include "vg_weapons.h"
#include "vg_shake.h"
#include "vg_sfx.h"
#include "vg_canopy_draw.h"

Weapons vg_wpn;

void vg_wpn_clear(void) {
    vg_wpn = Weapons{};
}

Threat vg_threat;

void vg_threat_clear(void) {
    vg_threat = Threat{};
}

// The player's side of a fight: acquiring a lock, spending a round, noticing one
// coming the other way, and taking a hit.
//
// Damage is HERE rather than with the collisions that cause it, because what a
// hit does to the player is a weapons-system question -- the grace period, the
// flash, the reference knock -- while what touched what is geometry. The
// collisions stayed in vg_game.cpp and call in through vg_damage_player and
// vg_kill_player, which were already the public entry points.

// ---------------------------------------------------------------------------
// Player weapons
// ---------------------------------------------------------------------------

// Acquire and hold a lock on whichever live enemy is nearest the nose, provided
// it stays inside the cone long enough.
void vg_update_lock(float dt) {
    // ACQUIRE IN THE CONE, HOLD IN THE VIEWPORT.
    //
    // These are two different questions and they were being answered by one test.
    // Acquiring is the aim -- put the nose on them and the shot exists, which is
    // the whole of what a sniper is asked to do. Holding only asks whether you
    // have lost them, and a ship that has drifted to the corner of the canopy has
    // not been lost: it is right there.
    //
    // Judged by one cone, one of the two always breaks. A cone tight enough to be
    // an aiming requirement drops targets that are plainly visible; a window wide
    // enough to keep them makes the aim free, which is what the last build did --
    // point roughly, fire, win.
    //
    // Tested in the ship's own frame rather than the camera's, so ROLL does not
    // decide it. Banking the picture should not drop a lock.
    int   best   = -1;
    float best_c = -2.0f;

    for (int i = 0; i < MAX_ENEMIES; i++) {
        const Ship* s = &vg.enemy[i];
        if (!s->alive) continue;
        float range = vlen(s->pos);
        if (range > vg.spec->lock_range || range < 1.0f) continue;
        float c = vdot(vnorm(s->pos), v3(0, 0, 1));   // player looks down +z

        // The held target is judged by the hold rule; everything else has to be
        // acquired, every time. So a lock that breaks costs the nose to get back,
        // which is what puts the choice between dodging and shooting back in.
        const bool holding = (i == vg_wpn.target && vg_wpn.locked);
        bool ok;
        if (!holding) {
            ok = (c > vg_lock_cos_at(vg.spec, range, false));
        } else if (vg.spec->lock_hold_cos < -1.0f) {
            ok = false;
            if (s->pos.z > NEAR_Z) {
                const float inv = FOCAL / s->pos.z;
                ok = fabsf(s->pos.x * inv) <= SCR_W * 0.5f
                  && fabsf(s->pos.y * inv) <= SCR_H * 0.5f;
            }
        } else {
            ok = (c > vg_lock_cos_at(vg.spec, range, true));
        }
        if (!ok) continue;

        // Still ranked by how near the nose it is, so the pick between two
        // eligible targets is unchanged -- only the eligibility test moved.
        if (c > best_c) { best_c = c; best = i; }
    }

    // Lock time scales with speed: acquiring is harder the faster you are going,
    // which is the trade the throttle is supposed to be. ACQUIRING -- not holding.
    // See the latch below.
    float sn = (vg.speed - vg.spec->speed_min)
             / (vg.spec->speed_max - vg.spec->speed_min);
    if (sn < 0.0f) sn = 0.0f;
    if (sn > 1.0f) sn = 1.0f;
    vg_wpn.lock_need = vg.spec->lock_time * (1.0f + LOCK_SPEED_PENALTY * sn);

    if (best < 0) {
        vg_wpn.target = -1;
        vg_wpn.lock_t      = 0;
        vg_wpn.locked      = false;
        vg_wpn.stacks      = 0;    // the bank goes with the lock
        vg_wpn.stack_t     = 0.0f;
        return;
    }

    if (best != vg_wpn.target) {
        vg_wpn.target = best;
        vg_wpn.lock_t      = 0;
        vg_wpn.locked      = false;
        vg_wpn.stacks      = 0;    // a new contact is a new bank
        vg_wpn.stack_t     = 0.0f;
    }
    vg_wpn.lock_t += dt;

    // ONCE EARNED, A LOCK IS HELD. It used to be re-evaluated from scratch every
    // frame against a threshold that rises with speed, so opening the throttle
    // after locking instantly revoked it -- the player was forbidden from using
    // the one control the fight is supposed to be about, at the exact moment they
    // had committed to an attack.
    //
    // That was the sharpest tooth in a set of three, all pushing the same way:
    // turn rate is 2.5x better at idle, acquisition is far slower at speed, and
    // this. Any one of them is a trade. Together they made "stop and shoot" not a
    // style but the only style, which is what the playtest found.
    //
    // The lock still drops the moment the target leaves the cone or the range, so
    // it is held by keeping the nose on them -- not by having once been fast
    // enough. Acquiring at speed is still hard; that trade is the point and it
    // stays.
    if (vg_wpn.lock_t >= vg_wpn.lock_need) vg_wpn.locked = true;

    // BANKING THE LOCK. A stacking class does not shoot when it has one, it saves
    // it -- see msl_stack_time. The bank fills only while the lock is actually
    // held, so the cost is unbroken contact rather than a precise cone.
    if (vg.spec->msl_stack_time > 0.0f) {
        if (vg_wpn.locked) {
            // AGAINST WHAT IS IN THE BAY, not against what the bay holds. Banking
            // a fifth lock with four rounds loaded is banking something that
            // cannot be fired: the salvo is already clamped to the rack at the
            // trigger, so the extra only ever showed as a filled cell that did
            // nothing. Worse on a part-spent rack -- two rounds left and four
            // cells lit is the instrument lying about what the press will do.
            vg_wpn.stack_t += dt;
            while (vg_wpn.stack_t >= vg.spec->msl_stack_time
                   && vg_wpn.stacks < vg_wpn.rounds) {
                vg_wpn.stack_t -= vg.spec->msl_stack_time;
                vg_wpn.stacks++;
            }
            if (vg_wpn.stacks >= vg_wpn.rounds) vg_wpn.stack_t = 0.0f;
            // A rack that shrank under a bank -- there is no path that does this
            // today, and a clamp costs nothing against one appearing later.
            if (vg_wpn.stacks > vg_wpn.rounds) vg_wpn.stacks = vg_wpn.rounds;
        } else {
            vg_wpn.stacks  = 0;
            vg_wpn.stack_t = 0.0f;
        }
    }
}

// The magazine refills ALL AT ONCE, and only from empty.
//
// It used to trickle a round back every `reload` seconds whenever the rack was
// below full, which quietly meant a class could never actually run dry: shoot
// four of six, wait, and you were topped up without ever having made a decision.
// A clip that refills while you are still shooting out of it cannot cost
// anything, so it cannot define a playstyle either.
//
// Now emptying the rack is the commitment. CHARIOT dumps twelve rounds in under
// two seconds and then has nine seconds of nothing; BALLISTA has three and has to
// mean all of them.
void vg_update_reload(float dt) {
    if (vg_wpn.rounds > 0 || vg_wpn.reload_t <= 0.0f) return;
    vg_wpn.reload_t -= dt;
    if (vg_wpn.reload_t <= 0.0f) {
        vg_wpn.reload_t = 0.0f;
        vg_wpn.rounds = vg.spec->magazine;
    }
}

void vg_player_fire(void) {
    if (vg_wpn.rounds <= 0 || vg_wpn.fire_gap > 0) return;
    if (!vg_wpn.locked || vg_wpn.target < 0) return;

    const Ship* s = &vg.enemy[vg_wpn.target];
    if (!s->alive) return;

    // HOW MANY LEAVE ON THIS PRESS. One for a class that fires singly; for a
    // stacking class, everything banked -- which is the whole trigger, and the
    // reason the aim it asks for is loose. A partial release is a real choice and
    // not a mistake: two now is often worth more than four in three seconds.
    int salvo = 1;
    if (vg.spec->msl_stack_time > 0.0f) {
        salvo = vg_wpn.stacks;
        if (salvo > vg_wpn.rounds) salvo = vg_wpn.rounds;
        if (salvo <= 0) return;
    }

    static int rail = 0;
    int sent = 0;
    for (int i = 0; i < salvo; i++) {
        // Alternate wing hardpoints so successive launches read as a pair.
        rail ^= 1;
        Vec3 origin = v3(rail ? 5.0f : -5.0f, -2.5f, 5.0f);
        // Only spend the round if a missile actually left the rail. With every
        // slot in the air this silently charged the player for nothing at all,
        // and since no missile existed there was no outcome to report either --
        // a shot that vanished in both directions.
        if (!vg_launch_missile(true, origin, vnorm(vsub(s->pos, origin)),
                               vg_wpn.target, -1, vg.spec))
            break;
        vg_wpn.rounds--;
        sent++;
    }
    if (sent == 0) return;

    vg_wpn.stacks   = 0;          // the bank is spent, whatever left the rail
    vg_wpn.stack_t  = 0.0f;
    vg_wpn.fire_gap = vg.spec->fire_gap;

    // LAUNCHING BREAKS THE LOCK, for a stacking class only.
    //
    // Without this the bank is permitted but never rewarded: one stack lands every
    // msl_stack_time and there is no reason on earth not to spend it immediately,
    // so the optimal play is a single round on that interval for ever -- which was
    // reported from the cockpit as spam, and is the exact opposite of the class.
    //
    // Making the launch cost the LOCK fixes the incentive rather than papering
    // over it with a longer trigger interval. The cycle becomes acquire, hold,
    // release, re-acquire, so spending a small bank throws away the contact that
    // earned it and holding for a fuller one is simply worth more. It is also the
    // honest reading of what a full bay leaving at once does to a seeker.
    if (vg.spec->msl_stack_time > 0.0f) {
        vg_wpn.locked = false;
        vg_wpn.lock_t = 0.0f;
    }
    vg_sfx_play(SFX_LAUNCH, 1.0f);

    // Emptying the rack starts the clock. Doing it here rather than in the tick
    // means the reload is timed from the shot that emptied it, not from the next
    // frame that happened to notice.
    if (vg_wpn.rounds <= 0) vg_wpn.reload_t = vg.spec->reload;
}

// Nearest live enemy missile tracking the player, for the threat warning.
void vg_update_threat(void) {
    vg_threat.on       = false;
    vg_threat.range = 1e9f;
    for (int i = 0; i < MAX_MISSILES; i++) {
        const Missile* m = &vg.msl[i];
        if (!m->alive || m->from_player || !m->locked) continue;
        float r = vlen(m->pos);
        if (r < vg_threat.range) {
            vg_threat.range = r;
            vg_threat.pos   = m->pos;
            vg_threat.on       = true;
        }
    }
}

// ---------------------------------------------------------------------------
// Damage
// ---------------------------------------------------------------------------

static bool s_player_hit = false;

bool vg_player_was_hit(void)  { return s_player_hit; }
void vg_clear_player_hit(void) { s_player_hit = false; }

// Any contact kills, whatever the hull is and whatever happened a moment ago.
//
// This deliberately ignores the VG_HIT grace period that vg_damage_player
// honours. That grace exists so one missile cannot cascade into three, which is
// a fairness rule about being SHOT. Flying into a wall while still flinching
// from a hit is not unfair, it is flying into a wall.
//
// VG_KILL still protects: at that point the opponent is already dead and the
// round is decided, and a win must not be taken back after the fact.
void vg_kill_player(void) {
    if (vg.state == VG_KILL) return;
    // BOTH. The hull cue carries the panel's damage beeps, and dying is the one
    // moment they most belong -- a collision comes straight through here without
    // ever touching vg_damage_player, so the loudest thing that can happen to the
    // player was also the quietest thing on the panel.
    vg_sfx_play(SFX_EXPLODE, 0.8f);
    vg_sfx_play(SFX_HIT, 1.0f);
    vg.health        = 0.0f;
    vg_cockpit.flash.hit     = 0.6f;
    // AMPED AND LINGERING, because the tearing is what carries the hit after the flash has
    // gone. DAMAGE_GLITCH alone was a blink; the panel going out lasts over a second and
    // the screen artifact used to be finished long before it.
    vg_cockpit.flash.glitch = DAMAGE_GLITCH * HIT_GLITCH_AMP;
    // AND A PANEL OF THE CANOPY GOES. Which one is the drawing's business -- see
    // vg_canopy_hit -- and it will not pick one that is already out.
    vg_canopy_hit();
    vg_shake_hit(1.35f);   // a kill lands harder than a wound
    s_player_hit     = true;
}

void vg_damage_player(float amount) {
    // Brief post-hit invulnerability, so one bad moment cannot cascade into three
    // hits before you have had a chance to react.
    //
    // VG_KILL is the same idea stretched: the round is decided and the loser is
    // mid-sentence, so a stray round still in the air must not be able to take
    // the win back after the fact.
    if (vg.state == VG_HIT || vg.state == VG_KILL) return;
    vg.health -= amount;
    if (vg.health < 0.0f) vg.health = 0.0f;
    vg_sfx_play(SFX_HIT, 1.0f);
    vg_cockpit.flash.hit     = 0.6f;
    // AMPED AND LINGERING, because the tearing is what carries the hit after the flash has
    // gone. DAMAGE_GLITCH alone was a blink; the panel going out lasts over a second and
    // the screen artifact used to be finished long before it.
    vg_cockpit.flash.glitch = DAMAGE_GLITCH * HIT_GLITCH_AMP;
    // AND A PANEL OF THE CANOPY GOES. Which one is the drawing's business -- see
    // vg_canopy_hit -- and it will not pick one that is already out.
    vg_canopy_hit();
    vg_shake_hit(1.0f);    // THE reference knock: everything else is scaled to it
    s_player_hit = true;
}
