#include "vg_sim.h"
#include "vg_shake.h"

// Fireballs and debris: everything an explosion is made of, and nothing that
// decides when one happens.
//
// Grouped by SUBJECT, which is the rule the renderer is already split on. The
// plan called this vg_spawn.cpp and would have swept in spawn_asteroid and
// spawn_enemy too, on the grounds that all three are spawning -- but that is a
// grouping by verb. An asteroid belongs to the world and a fighter belongs to
// the match, and neither has anything to say to a shard of hull. What these do
// share is a pool, a lifetime and a look, and they get edited together every
// time the explosions are tuned.
//
// vg_spawn_blast is the one that reaches outside itself: it raises the cockpit
// flash, the knock and the panel glitch, because those are consequences of an
// explosion and no caller should have to remember them.

// `out` is how far from the centre the shards START. It is separate from
// `radius`, which only scales how LONG each shard is, because the two want
// opposite things in an explosion: short bright streaks, launched from outside
// the fireball. At 0.35 * radius they were spawning deep inside the burning
// part, so by the time they were bright enough to see, a sphere had opened over
// the top of them and they were never visible leaving.
static void spawn_shards(Vec3 at, float radius, float out, int count,
                         float speed_k, float life_k) {
    for (int k = 0; k < count; k++) {
        Debris* d = nullptr;
        for (int i = 0; i < MAX_DEBRIS; i++) if (!vg.deb[i].alive) { d = &vg.deb[i]; break; }
        if (!d) return;
        Vec3 dir = vg_rand_unit();
        d->alive = true;
        // Jittered, so the launch is a rough shell and not a geometric ring
        // popping into existence at one distance.
        d->pos   = vadd(at, vmul(dir, out * vg_frand(0.80f, 1.25f)));
        // POINTING THE WAY IT IS GOING, and going that way. seg used to be
        // oriented at random, which reads as a cloud of tumbling litter: a
        // streak lying across its direction of travel says spinning, a streak
        // lying along it says thrown. Both the shard's axis and its velocity now
        // come off the same outward bearing, so the whole field is radial lines
        // leaving a centre.
        //
        // A little jitter into the axis on purpose. Exactly radial is a starburst,
        // which is a diagram; a few degrees of slop is wreckage.
        const Vec3 axis = vnorm(vadd(dir, vmul(vg_rand_unit(), 0.16f)));
        d->seg   = vmul(axis, radius * vg_frand(0.5f, 1.0f));
        d->vel   = vmul(axis, vg_frand(11.0f, 34.0f) * speed_k);
        d->life0 = vg_frand(0.40f, 1.00f) * life_k;
        d->life  = d->life0;
    }
}

void vg_spawn_debris(Vec3 at, float radius, int count) {
    // The old launch distance, so a scrape looks exactly as it did.
    spawn_shards(at, radius, radius * 0.35f, count, 1.0f, 1.0f);
}

// NOTHING SLOWS IT DOWN OUT HERE. A collision throws shards at walking pace and
// they are gone in half a second, which is right for a scrape. A hull letting go
// is the other thing entirely: the pieces leave fast, they leave in every
// direction because there is no ground to fall towards, and they keep going
// until they are out of sight. That is a speed and a lifetime, not a bigger
// radius, which is why this takes both.
void vg_spawn_shrapnel(Vec3 at, float radius, float out, int count,
                       float speed_k, float life_k) {
    spawn_shards(at, radius, out, count, speed_k, life_k);
}

void vg_spawn_fireball(Vec3 at, Vec3 vel, float radius, float life_k) {
    Fireball* f = nullptr;
    for (int i = 0; i < MAX_FIREBALLS; i++) if (!vg.fire[i].alive) { f = &vg.fire[i]; break; }
    if (!f) return;
    f->alive = true;
    f->pos   = at;
    f->vel   = vel;
    f->r     = radius;
    // Two independent rolls, deliberately: how long it lasts and how it goes out
    // are different questions. A short-lived ball that holds its brightness and a
    // long-lived one that collapses early both want to exist in the same cluster.
    f->fall  = vg_frand(0.50f, 2.60f);
    f->life0 = vg_frand(FIRE_LIFE_MIN, FIRE_LIFE_MAX) * life_k;
    f->life  = f->life0;
}

// SEVERAL BALLS, NOT ONE BIGGER ONE. A single sphere reads as a bubble however
// large it is drawn. Three or six of them at scattered offsets and staggered
// sizes overlap into a shape with lumps in it, and because each one runs its own
// life the cluster brightens and cools unevenly -- which is the thing that makes
// it read as burning rather than as an expanding circle.
void vg_spawn_blast(Vec3 at, float radius, int balls, int shards, float life_k) {
    // SCATTERED WIDE AND DRIFTING OUTWARD. Clustered at the centre with a random
    // drift, the balls overlapped into one blob that sat still -- which is a
    // flare, not an area coming apart. Each one now starts somewhere out along
    // its own bearing and keeps going that way, so the group opens up over its
    // life and the gaps between the balls are part of the shape.
    for (int k = 0; k < balls; k++) {
        const Vec3 dir = vg_rand_unit();
        const Vec3 off = vmul(dir, radius * vg_frand(0.15f, 0.85f));
        vg_spawn_fireball(vadd(at, off),
                          vmul(dir, vg_frand(7.0f, 24.0f) * life_k),
                          radius * vg_frand(0.45f, 1.0f), life_k);
    }
    // Shards leave faster and last longer the bigger the event was, for the
    // reason in vg_spawn_shrapnel.
    // Launched from the fireball's own rim, not its middle, and thrown hard --
    // nothing decelerates it, so the distance it covers is the whole read on how
    // much energy came out. The old multiplier had shards travelling about as far
    // as the fireball itself expanded, which is why they never appeared to leave.
    if (shards > 0)
        spawn_shards(at, radius, radius * 1.15f, shards,
                     2.2f + 1.4f * life_k, life_k);

    // LIGHT ARRIVES. Positions in this game are player-relative, so vlen(at) IS
    // the range to the cockpit -- an explosion beside you throws light on the
    // panel and one across the arena does not. Held as a maximum rather than
    // summed: two blasts in the same frame are one flash, because the eye is
    // being told "something went off near you" and not asked to count.
    const float rng = vlen(at);
    float lit = radius * 6.0f / (rng + 60.0f);
    if (lit > 0.85f) lit = 0.85f;
    if (lit > vg_cockpit.flash.blast) vg_cockpit.flash.blast = lit;

    // AND SO DOES THE PRESSURE. Its own curve rather than reusing the light's:
    // a flash carries much further than a knock does, so this falls off harder
    // and is felt across a smaller part of the arena. Unlike the flash it
    // ACCUMULATES -- see vg_shake.h -- because two warheads either side of the
    // canopy really should be worse than one.
    float knock = radius * 5.0f / (rng + 90.0f);
    if (knock > 1.20f) knock = 1.20f;
    vg_shake_hit(knock);

    // Close enough to hurt the display. Well short of the fireball's own radius,
    // so this is "it went off next to you" rather than "you are inside it" --
    // that case is handled in the world step, where the fireball is still around
    // to be inside OF.
    if (knock > 0.75f) {
        const float g = DAMAGE_GLITCH * (knock - 0.75f) * 2.2f;
        if (g > vg_cockpit.flash.glitch) vg_cockpit.flash.glitch = g;
    }
}

int vg_fire_live(void) {
    int n = 0;
    for (int i = 0; i < MAX_FIREBALLS; i++) if (vg.fire[i].alive) n++;
    return n;
}
