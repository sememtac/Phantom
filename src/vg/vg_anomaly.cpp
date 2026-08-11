#include "vg_anomaly.h"
#include "vg_event.h"
#include "vg_arena.h"
#include "vg_shake.h"
#include "vg_ift.h"
#include "vg_config.h"

// WHAT THE AMPLITUDE MEANS, which is the only thing vg_event does not know.
//
// Two things, and the second is the one that costs the player something. The moving wall
// takes away room; the rumble takes away the shot. Both ride the SAME eased amplitude, so
// the shaking arrives with the ground moving and settles with it, and a mild episode is
// mild in both.
static void anom_apply(float amp) {
    vg_arena_warp_set(amp);
    // ON THE RUMBLE CHANNEL because that is exactly what this is -- a condition, renewed
    // every frame it holds, where the loudest reason wins instead of summing. Flying through
    // a wreck during an anomaly should not be the two of them added together.
    //
    // Requested after vg_shake_update has already run this frame, so it lands on the next
    // one. PASS_RUMBLE does the same thing from vg_world_step's tail, and one frame is
    // nothing to a condition that holds for half a minute.
    //
    // A request of 0 is a no-op rather than a silence -- vg_shake_rumble keeps the loudest --
    // which is why the channel needs no clearing when an episode ends. The warp does, and
    // gets it, because it is a value and not a request.
    vg_shake_rumble(ANOM_RUMBLE * amp);
}

// THE FIRST THING TO EARN THE BROADCAST A VOICE MID-FIGHT, and the exception is narrow: the
// arena changing shape cannot be read off the instruments, and a pilot would not say it,
// being busy and not the authority on the venue. See the note in vg_ift.h.
static void anom_announce(bool arriving) {
    vg_ift_line(arriving ? IFT_ANOMALY : IFT_ANOMALY_END);
}

static const MatchEventSpec ANOM_SPEC = {
    ANOM_CHANCE_ROUND0, ANOM_CHANCE_FINAL,
    ANOM_CHANCE_SECOND,
    ARENA_WARP_ROUND0,
    ANOM_WAIT_MIN,  ANOM_WAIT_MAX,
    ANOM_HOLD_MIN,  ANOM_HOLD_MAX,
    ANOM_ONSET_S,   ANOM_FADE_S,
    ANOM_PEAK_MIN,  ANOM_PEAK_MAX,
    anom_apply,
    anom_announce,
};

static MatchEvent s_anom;

void vg_anomaly_begin_match(float round_t) {
    vg_event_begin(&s_anom, &ANOM_SPEC, round_t);
}

void vg_anomaly_step(float dt, bool frozen) {
    vg_event_step(&s_anom, &ANOM_SPEC, dt, frozen);
}

// Not routed through the event, because it is not an event thing: the menus want a round
// tube whether or not a match was ever played, and the schedule has nothing to say about
// that. It leaves s_anom alone -- a match that ends mid-episode is over, and vg_match_start
// rerolls the next one from scratch.
void vg_anomaly_clear(void) {
    vg_arena_warp_set(0.0f);
}
