#include "vg_event.h"
#include "vg_sim.h"

// The clock and the dice. Nothing here knows what an episode does to the world -- see
// the note at the top of vg_event.h for where that line is drawn and why.

// The amplitude a round is allowed to reach at full strength.
static inline float event_round_amp(const MatchEventSpec* spec, float round_t) {
    return spec->strength_round0 + (1.0f - spec->strength_round0) * round_t;
}

void vg_event_begin(MatchEvent* ev, const MatchEventSpec* spec, float round_t) {
    ev->phase   = EV_OFF;
    ev->t       = 0.0f;
    ev->dur     = 0.0f;
    ev->peak    = 0.0f;
    ev->left    = 0;
    ev->round_t = (round_t < 0.0f) ? 0.0f : (round_t > 1.0f ? 1.0f : round_t);
    // THE VENUE LAUNCHES CLEAN, whatever is coming. A disturbance already present at launch
    // would be a property of the map; arriving partway through a fight makes it an event.
    if (spec->apply) spec->apply(0.0f);

    const float chance = spec->chance_round0 +
                         (spec->chance_final - spec->chance_round0) * ev->round_t;

    // THREE ROLLS, IN THIS ORDER, and the order is behaviour rather than style: the stream is
    // recorded, so moving a roll re-weathers every session recorded before the move. A quiet
    // match takes only the first of them -- see vg_event.h.
    if (vg_frand(0.0f, 1.0f) >= chance) return;     // a quiet match, and that is a result

    ev->left  = (vg_frand(0.0f, 1.0f) < spec->chance_second) ? 1 : 0;
    ev->phase = EV_WAIT;
    ev->dur   = vg_frand(spec->wait_min, spec->wait_max);
}

void vg_event_step(MatchEvent* ev, const MatchEventSpec* spec, float dt, bool frozen) {
    if (ev->phase == EV_OFF) return;
    if (frozen) return;

    ev->t += dt;

    switch (ev->phase) {
    case EV_WAIT:
        if (ev->t >= ev->dur) {
            ev->phase = EV_ONSET;
            ev->t     = 0.0f;
            ev->peak  = event_round_amp(spec, ev->round_t) *
                        vg_frand(spec->peak_min, spec->peak_max);
            // ANNOUNCED AS IT STARTS, not before and not after. Before would be a warning,
            // which would make the broadcast an authority on something it may not understand;
            // after would be a report of something the player has already flown into. Said as
            // the ground begins to move, it reads as the broadcast noticing at the same moment
            // the pilot does.
            if (spec->announce) spec->announce(true);
        }
        break;

    case EV_ONSET:
        if (ev->t >= spec->onset_s) {
            ev->phase = EV_HOLD;
            ev->t     = 0.0f;
            ev->dur   = vg_frand(spec->hold_min, spec->hold_max);
        }
        break;

    case EV_HOLD:
        if (ev->t >= ev->dur) {
            ev->phase = EV_FADE;
            ev->t     = 0.0f;
            if (spec->announce) spec->announce(false);
        }
        break;

    case EV_FADE:
        if (ev->t >= spec->fade_s) {
            if (ev->left) {
                ev->left--;
                ev->phase = EV_WAIT;
                ev->t     = 0.0f;
                ev->dur   = vg_frand(spec->wait_min, spec->wait_max);
            } else {
                ev->phase = EV_OFF;
            }
        }
        break;

    default: break;
    }

    // ONE PLACE SETS THE AMPLITUDE, whatever the phase decided. A smoothstep on both ends, so
    // the world does not start or stop moving on a corner.
    //
    // Reached with the phase the switch above just moved to, which is what makes the frame an
    // episode ends the frame the handler is called with 0. After that the early return keeps it
    // from being called at all, and a handler must leave the world alone when it is not asked.
    float k = 0.0f;
    switch (ev->phase) {
    case EV_ONSET: k = ev->t / spec->onset_s;         break;
    case EV_HOLD:  k = 1.0f;                          break;
    case EV_FADE:  k = 1.0f - ev->t / spec->fade_s;   break;
    default:       k = 0.0f;                          break;
    }
    if (k < 0.0f) k = 0.0f; else if (k > 1.0f) k = 1.0f;
    k = k * k * (3.0f - 2.0f * k);
    if (spec->apply) spec->apply(ev->peak * k);
}
