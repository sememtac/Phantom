#include "vg_surge.h"
#include "vg_event.h"
#include "vg_config.h"

Surge vg_surge;

// WHAT THE AMPLITUDE MEANS. One assignment, because this event's whole job is to be a
// number three places in vg_render can see -- and the reason it cannot do more than that
// is written up in vg_surge.h.
//
// Called with 0 on the frame the episode ends, which is what puts the panel back. That is
// vg_event's contract and not an accident of this handler: a handler must be safe to call
// with 0 at any time, including before anything has started.
static void surge_apply(float amp) {
    vg_surge.level = amp;
}

// IT SAYS NOTHING, and that is the author's decision about what the broadcast IS.
//
// It had a pair of lines and they were wrong. The IFT reports on the VENUE -- the arena
// itself distorting is information the player cannot get anywhere else, and cannot read off
// the instruments. A ship's own panel failing is not news: it is the player's problem, and
// announcing it makes the broadcast an authority on the inside of somebody's cockpit.
//
// It also stopped a real annoyance. The two events are independent, so a match with both
// produced four announcements over one fight, and the anomaly's two -- the only ones that
// carry information -- were competing with two that did not.
//
// vg_event allows this: `announce` may be null for an event nobody announces. Nothing in
// the mechanism had to change to make an event silent, which is the interface working.

// Thirteen numbers in MatchEventSpec's order, all of them from cfg_events.h. Laid out on
// the same lines as ANOM_SPEC in vg_anomaly.cpp so the two can be read side by side and a
// third event is a copy of either.
static const MatchEventSpec SURGE_SPEC = {
    SURGE_CHANCE_ROUND0, SURGE_CHANCE_FINAL,
    SURGE_CHANCE_SECOND,
    SURGE_ROUND0,
    SURGE_WAIT_MIN,  SURGE_WAIT_MAX,
    SURGE_HOLD_MIN,  SURGE_HOLD_MAX,
    SURGE_ONSET_S,   SURGE_FADE_S,
    SURGE_PEAK_MIN,  SURGE_PEAK_MAX,
    surge_apply,
    nullptr,        // says nothing -- see above
};

static MatchEvent s_surge;

void vg_surge_begin_match(float round_t) {
    vg_event_begin(&s_surge, &SURGE_SPEC, round_t);
}

void vg_surge_step(float dt, bool frozen) {
    vg_event_step(&s_surge, &SURGE_SPEC, dt, frozen);
}

// Not routed through the event, for the same reason vg_anomaly_clear is not: the menus
// want honest instruments whether or not a match was ever flown, and the schedule has
// nothing to say about that. It leaves s_surge alone -- a match that ends mid-episode is
// over, and vg_surge_begin_match rerolls the next one from scratch.
void vg_surge_clear(void) {
    vg_surge.level = 0.0f;
}
