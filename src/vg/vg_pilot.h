#pragma once
#include <stdint.h>

// ===========================================================================
// WHO IS FLYING IT.
//
// ShipSpec says what an airframe can do. This says what the pilot in it is
// like, and the two are deliberately separate: the same CHARIOT flown by a
// nervous rookie and by somebody who has been doing this for years should be
// two different fights, and neither of them should require a second CHARIOT.
//
// BORROWED FROM QUAKE 3 ARENA, and it is worth saying what was and was not
// taken. Most of that bot system is an Area Awareness System -- a compiler that
// turns a static map into areas, reachability links and travel times, so a bot
// can work out how to get to the rocket launcher. None of it applies here:
// there is no level to navigate, no items, no inventory and no objectives, only
// open space and two aeroplanes.
//
// What DOES apply is the idea underneath the character files: a bot's
// personality is a TABLE OF NAMED TRAITS, not code. Q3 ships dozens of them per
// bot, interpolated across five skill tiers, and that is why its bots read as
// people rather than as difficulty settings. The traits below are the ones this
// game has any use for.
//
// AND THE OTHER HALF OF THE IDEA: BEING BAD IS A MODEL, NOT AN ABSENCE.
//
// Every handicap an enemy had was a gate -- slow enough to shoot, nose inside a
// cone, rounds in the rack -- and inside those gates they were PERFECT. skill
// scaled their turn rate, so a poor pilot flew a worse aeroplane rather than
// flying it worse. Nothing modelled pointing slightly off, or taking a moment
// to notice. That is why they read as correct rather than as deciding, and it
// is what `aim` and `reaction` are here to fix.
// ===========================================================================

struct PilotSpec {
    // Three letters, for the bracket and for a debug line. Not a callsign: the
    // entrants own those, and one archetype is flown by many pilots.
    const char* name;

    // Turn rate, as a scale. 1.0 is as good as the player's airframe allows.
    // This is the trait the game already had, under the name ENEMY_SKILL.
    float fly;

    // HOW STEADILY THEY POINT, in radians of wander either side of the nose.
    //
    // Applied as a slow drift of the direction the ship aims WITH, not of the
    // direction it flies -- so it costs them lock time, breaks locks they had,
    // and sends rounds off the rail a little wrong, all from one mechanism and
    // all visible from the cockpit. Zero is a machine.
    float aim;

    // ...and how often that drift is re-rolled. Slower is worse to fight than
    // faster at the same amplitude: a fast wobble averages out over the length
    // of a lock, where a slow one sits off-target for the whole of it.
    float aim_hold;

    // HOW LONG BEFORE THEY ANSWER SOMETHING NEW, in seconds. A missile in the
    // air, somebody arriving on their tail. Not a delay on flying -- they keep
    // doing whatever they were doing until this expires, which is exactly what
    // being slow to notice looks like from outside.
    float reaction;

    // Break range and extend time, as a scale. Below 1 leaves early and stays
    // away; above 1 stays in past the point the geometry says to go. Was rolled
    // per ship as a bare random; it belongs to the pilot.
    float nerve;

    // Willingness to hold a won position rather than break off it, as a scale
    // on how long a pursuit may run. A cautious pilot takes the shot and leaves;
    // somebody confident sits there.
    float press;

    // HOW HARD THEY COMMIT, 0 to 1, and it only means anything to a pilot flown
    // by the network.
    //
    // A trained policy is a copy of whoever was recorded, INCLUDING how careful
    // they were. The first one flew a BALLISTA at a mean throttle of 0.286 and
    // won by picking shots, so the policy picks shots too -- measured against the
    // hand-written AI it points at the player far more (+0.393 against -0.320)
    // and keeps five times as many rounds in the air, and still reads as passive
    // from the cockpit. It hovers. It never comes.
    //
    // What is missing is not skill, it is COMMITMENT, and that is a property of
    // the person and not of the flying. So the network says where to point and
    // this says how much of the distance to close while doing it. The two are
    // separable in a way that is worth keeping separable: retraining changes the
    // flying, and this stays the dial for what kind of pilot is doing it.
    float aggression;
};

// THE TIERS DO NOT SEPARATE A PILOT THE NETWORK IS FLYING, and that is measured
// rather than suspected. Paired across four fixed seeds, against the class where
// the network is demonstrably better than the rules:
//
//     RAW  1.33%    PRO  1.85%    ACE  1.44%     (hull taken off the player)
//     ACE minus RAW, paired: +0.11, spread 0.63
//
// Two of the four seeds came out IDENTICAL across all three tiers. The reason is
// structural: most of this table modulates the class TACTIC, and a network
// replaces the tactic outright. Only `fly` and `aim` still reach it, through the
// turn rate and the lock.
//
// So difficulty tiering works on the hand-written opponents and is close to
// inert on the trained ones. Anything that wants a ladder for a trained pilot
// has to come from the training, not from here.



#define PILOT_KINDS 5

extern const PilotSpec vg_pilot_kind[PILOT_KINDS];

// The pilot for an entrant of this rating, 0..1. The bracket's seeding IS the
// difficulty curve, so this is the one place the two are tied together: a first
// round opponent is somebody's first tournament, and the far side of the draw
// is not.
const PilotSpec* vg_pilot_for(float rating);

// The one flown when nothing has said who is flying -- the gym, the bench, an
// opponent with no bracket behind them. Deliberately the middle of the table
// rather than the best of it: a test rig should be a fair fight, not a wall.
const PilotSpec* vg_pilot_default(void);
