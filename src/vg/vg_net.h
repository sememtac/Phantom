#pragma once

// ===========================================================================
// THE TRAINED PILOT, RUN ON THE BOARD.
//
// Weights come from tools/train_pilot.py as a generated header, beside the
// canopy tables and read the same way: a build input, produced by a tool,
// checked in because the firmware cannot be built without it.
//
// WHAT IT GIVES IS A TARGET, NOT A STICK POSITION, and that is a measured
// result rather than a preference. A pilot's stick matches its own previous
// position to 0.998 from one frame to the next, so a network asked for the
// position each frame learns to copy the last one and nothing else. The signal
// only appears further out: below about half a second the last position is the
// better guess, and past it the view out of the canopy is. So this says where
// the stick should be over the next couple of seconds, and the caller moves
// toward it the way a hand does.
// ===========================================================================

struct VgNetOut {
    float pitch;      // -1..1
    float yaw;        // -1..1
    float throttle;   // 0..1
    // WHETHER TO SHOOT, already decided. The raw output is a probability that
    // the pilot fires within the horizon, and the cut that turns it into a yes
    // is calibrated by the trainer rather than assumed -- see PILOT_FIRE_T.
    //
    // It is a WISH and not a launch. Every gate the weapon has still applies: a
    // lock has to exist, the rack has to hold something, the trigger has its own
    // interval. This says the pilot wants to shoot, which is the half a rule
    // could never judge.
    bool  fire;
    // False when the network was not run. The one case that matters is a width
    // mismatch: the observation grew and nobody retrained.
    bool  valid;
};

// Whether a network is compiled in at all.
bool vg_net_available(void);
// The width it expects, for the caller to check against its own observation.
int  vg_net_inputs(void);
int  vg_net_weights(void);

// DOES IT KNOW THIS SHIP?
//
// A policy asked to fly a class it never saw does not refuse -- it guesses, and
// it guesses by flying the class it does know. Measured before there was any way
// to ask: a network fitted to BALLISTA recordings flew all four like a BALLISTA,
// and the attract demo had to be pinned to one class to hide it.
//
// The trainer writes down which airframes appeared in its data, so the question
// has an answer that cannot go stale. A class that was never recorded is flown
// by the hand-written tactics instead, which is the right outcome: a good rule
// beats a confident guess.
bool vg_net_knows_ship(const float* obs, int n);

// Run it. `n` is the caller's observation width and is checked, not trusted.
void vg_net_run(const float* obs, int n, VgNetOut* out);
