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
    // False when the network was not run. The one case that matters is a width
    // mismatch: the observation grew and nobody retrained.
    bool  valid;
};

// Whether a network is compiled in at all.
bool vg_net_available(void);
// The width it expects, for the caller to check against its own observation.
int  vg_net_inputs(void);
int  vg_net_weights(void);

// Run it. `n` is the caller's observation width and is checked, not trusted.
void vg_net_run(const float* obs, int n, VgNetOut* out);
