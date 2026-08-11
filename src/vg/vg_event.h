#pragma once
#include <stdint.h>

// ===========================================================================
// EPISODIC MATCH EVENTS: something happens to the venue, partway through a fight
//
// One clock, five phases and one amplitude, with everything it does to the world
// behind two function pointers. The ARENA ANOMALY is the first of these -- see
// vg_anomaly.cpp -- and this file exists because it should not be the only one.
// It was written as a bespoke state machine inside vg_game.cpp, and a second event
// would have had to copy the dice, the clock, the easing and the freeze rule to get
// the same pacing. Those four are the parts that are not about any particular event.
//
// WHAT IS GENERIC AND WHAT IS NOT. The schedule is generic: a match either has an
// episode or it does not, it arrives after a wait rather than at launch, it comes on
// faster than it goes away, and it may come back once. What the amplitude MEANS is
// not, and this file never learns -- it hands a number to `apply` and does not ask
// what happens to it.
//
// IT KNOWS NOTHING ABOUT THE TOURNAMENT either. The round arrives as a 0..1 ramp the
// caller works out, because the bracket is vg_tourney's business and an event module
// that included it would be reaching two subsystems sideways to ask what match this
// is. See vg_anomaly_begin_match.
//
// THE DICE COME FROM vg_frand, which is the recorded stream, so a replay reproduces
// the same weather. The ORDER of the rolls is therefore part of the behaviour and not
// an implementation detail: changing when this file asks for a number changes every
// recording made before the change. See vg_replay.
// ===========================================================================

enum EventPhase : uint8_t {
    EV_OFF = 0,   // this match is quiet, or the last episode is over
    EV_WAIT,      // an episode is coming, and this is the calm before it
    EV_ONSET,     // ramping up; the broadcast has already said so
    EV_HOLD,      // at strength
    EV_FADE       // ramping down
};

// The constants that make one event different from another. All of it is const data
// -- an event is a table row plus two functions, which is the whole point.
struct MatchEventSpec {
    // The chance a match has any episode at all, at round 0 and at the final. A quiet
    // match is part of the mechanic: an event that happened every time would be scenery.
    float chance_round0, chance_final;
    // A match that has one can have a second.
    float chance_second;
    // The amplitude a round-0 match is allowed to reach, against the final's 1.0.
    // Difficulty: the last round should be flown somewhere less forgiving than the first.
    float strength_round0;
    // Seconds of quiet before an episode, and how long it stays once it has arrived.
    float wait_min, wait_max;
    float hold_min, hold_max;
    // NOT SYMMETRICAL, and that asymmetry is the whole read of an episode. Coming on fast
    // enough to notice mid-turn is what makes it an event; going away slowly is what makes
    // it feel like something passing rather than something switched off.
    float onset_s, fade_s;
    // An episode does not always reach the round's full strength, so two matches in the
    // same round are not the same weather.
    float peak_min, peak_max;

    // WHAT THE EVENT DOES, called every frame an episode is live and once with 0 on the
    // frame it ends. `amp` is the eased amplitude, already scaled by the round and by this
    // episode's peak, so a handler is one line per thing it drives and has no arithmetic
    // of its own.
    //
    // Called with 0 rather than simply not called, because that is the only way a handler
    // can put the world back without a second hook to do it. A handler must therefore be
    // safe to call with 0 at any time, including before anything has started.
    void (*apply)(float amp);

    // The broadcast noticing. `arriving` is true as the ramp starts and false at the top
    // of the fade -- said while the ground is still settling, so the player gets to watch
    // it be true. May be null for an event nobody announces.
    void (*announce)(bool arriving);
};

// Where one event has got to. Plain state, owned by whoever declares it -- the scheduler
// holds no statics, so two events run side by side without knowing about each other.
struct MatchEvent {
    EventPhase phase;
    float      t;        // seconds spent in this phase
    float      dur;      // how long WAIT or HOLD lasts
    float      peak;     // the amplitude this episode reaches, 0..1
    // KEPT FROM vg_event_begin rather than asked for again at each onset. The round cannot
    // change mid-match, and holding it here is what lets the step take no tournament argument
    // at all -- the caller answers the question once, where it already knows the answer.
    float      round_t;  // 0 at the first round, 1 at the final
    uint8_t    left;     // episodes still to come after this one
};

// Roll the whole match's weather before it starts, and put the world back to nothing.
//
// DECIDING IT UP FRONT rather than rolling every second is what lets a match be genuinely
// quiet: a per-frame chance always fires eventually, and "eventually" over a four minute
// fight is every time.
//
// `round_t` is 0 at the first round and 1 at the final, and is what both the chance and the
// strength are interpolated along.
void vg_event_begin(MatchEvent* ev, const MatchEventSpec* spec, float round_t);

// Advance one frame. Cheap and safe to call every frame of every state; it returns at once
// when there is nothing running.
//
// `frozen` HOLDS THE CLOCK WITHOUT ENDING THE EPISODE, and it exists for one moment: after
// a kill, the broadcast is busy. There is only ever one IFT line up, so an event announcing
// itself while the round is being summed up would delete the summary or be deleted by it.
// The amplitude simply holds where it was, and whatever puts the venue back on the way out
// of a match flattens it. See vg_use_menu_sky.
void vg_event_step(MatchEvent* ev, const MatchEventSpec* spec, float dt, bool frozen);
