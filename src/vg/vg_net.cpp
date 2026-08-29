#include "vg_net.h"
#include "vg_modes.h"
#include "generated/pilot_net.h"
#include <math.h>

// ===========================================================================
// THE FORWARD PASS, AND NOTHING ELSE.
//
// Three layers of multiply-and-add. No framework, no memory arena, no graph:
// the whole network is six thousand weights in flash and a hundred and thirty
// floats of stack, and a library to run it would be larger than the thing it
// ran.
//
// WHAT IT COSTS, and it is worth writing down because the answer surprises
// people. 27x64 + 64x64 + 64x3 is 6016 multiply-adds. The S3 has a hardware
// float unit and runs at 240 MHz, so the arithmetic is tens of microseconds
// against a frame budget of 16,600. The whole of it bills to `ai` in the
// telemetry, which was reading 0 before this existed.
//
// THE ACTIVATION IS THE EXPENSIVE PART, not the arithmetic. There are 128
// hidden units and tanhf() is a library call into a transcendental, which can
// cost more than the sixty-four multiply-adds that produced its argument. See
// fast_tanh below.
// ===========================================================================

// A rational approximation of tanh, accurate to about 1e-3 over the range that
// matters, which is far inside the noise of a policy that was fitted to a human
// hand.
//
// NOT AN OPTIMISATION MADE ON SUSPICION. The library call was measured first;
// this replaced it only after the measurement said the activations cost more
// than the multiply-adds. The exact form is the standard Pade fit: it is
// monotonic, it saturates at the right values, and it has no branch except the
// clamp.
static inline float fast_tanh(float x) {
    // Beyond this the fit drifts and the true function is flat anyway.
    if (x < -3.0f) return -1.0f;
    if (x >  3.0f) return  1.0f;
    const float x2 = x * x;
    return x * (27.0f + x2) / (27.0f + 9.0f * x2);
}

// One layer: out = act(W * in + b). W is stored row by row, which is the layout
// torch writes -- row j holds the weights of output j.
static void layer(const float* w, const float* b, const float* in, float* out,
                  int n_in, int n_out, bool activate) {
    for (int j = 0; j < n_out; j++) {
        const float* row = w + (size_t)j * n_in;
        float a = b[j];
        for (int i = 0; i < n_in; i++) a += row[i] * in[i];
        out[j] = activate ? fast_tanh(a) : a;
    }
}

bool vg_net_available(void) { return true; }

bool vg_net_knows_ship(const float* obs, int n) {
#if defined(PILOT_SHIPS) && PILOT_SHIPS > 0
#  ifndef PILOT_SHIP_TOL
#    define PILOT_SHIP_TOL 0.005f   // an older header that did not record one
#  endif
    if (!obs || n < PILOT_NET_IN) return false;
    // The airframe fields are the LAST PILOT_SHIP_N of the observation, which is
    // the layout rule again: they were appended, so they stay at the end.
    const float* mine = obs + (n - PILOT_SHIP_N);
    for (int k = 0; k < PILOT_SHIPS; k++) {
        const float* row = PILOT_SHIP_SEEN + (size_t)k * PILOT_SHIP_N;
        bool same = true;
        for (int i = 0; i < PILOT_SHIP_N && same; i++) {
            float d = mine[i] - row[i];
            if (d < 0.0f) d = -d;
            // THE TOLERANCE IS THE JITTER THE NETWORK WAS TRAINED THROUGH, and
            // that is the whole reason the jitter is worth having.
            //
            // Trained on exact values, this is a rounding allowance, and then the
            // gate is brutal: measured, moving one class's hull by 11% -- an
            // ordinary tuning pass -- put the ship outside the list, the network
            // declined, and its whole advantage went back to the rules. The
            // policy had not got worse. It had stopped being asked.
            //
            // Trained through a spread of tables, the gate can accept that same
            // spread, because those are tables it has actually flown.
            if (d > PILOT_SHIP_TOL) same = false;
        }
        if (same) return true;
    }
    return false;
#else
    (void)obs; (void)n;
    return true;   // an older header that did not record what it saw
#endif
}
int  vg_net_inputs(void)    { return PILOT_NET_IN; }
int  vg_net_weights(void)   { return PILOT_NET_IN * PILOT_NET_H + PILOT_NET_H
                                   + PILOT_NET_H * PILOT_NET_H + PILOT_NET_H
                                   + PILOT_NET_H * PILOT_NET_OUT + PILOT_NET_OUT; }

void vg_net_run(const float* obs, int n, VgNetOut* out) {
    if (!obs || !out) return;

    // A LAYOUT MISMATCH IS SILENT, so it is checked. The observation and the
    // weights are both just arrays of floats: feed one to the other with the
    // widths out of step and it runs happily and flies into the ground.
    //
    // THE WIDTH MUST MATCH EXACTLY, and this used to be generous.
    //
    // A wider observation was once accepted, on the rule that new fields go at the
    // END so the leading values keep their meaning. The rule is sound and still
    // holds -- but it is a rule about where a future edit puts things, and it was
    // enforced by nothing except a comment.
    //
    // It got broken. The observation grew a block that had to go in FRONT of the
    // eleven airframe fields, because vg_net_knows_ship finds those by counting
    // back from the end. An old header then read the new fields as airframe and
    // flew on the difference -- and it passed the ship gate the whole time, since
    // the gate counts from the other end and was still perfectly correct. Those
    // fields were removed again for measuring worse than nothing; the hole they
    // went through was not.
    //
    // So the width is checked in both directions now. A mismatch declines, the
    // tactics fly, and a layout mistake costs a network instead of hiding inside
    // one.
    if (n != PILOT_NET_IN) {
        out->valid = false;
        return;
    }

    float x[PILOT_NET_IN];
    for (int i = 0; i < PILOT_NET_IN; i++)
        x[i] = (obs[i] - PILOT_IN_MEAN[i]) / PILOT_IN_STD[i];

    float h0[PILOT_NET_H];
    float h1[PILOT_NET_H];
    float y[PILOT_NET_OUT];
    layer(PILOT_W0, PILOT_B0, x,  h0, PILOT_NET_IN, PILOT_NET_H, true);
    layer(PILOT_W1, PILOT_B1, h0, h1, PILOT_NET_H,  PILOT_NET_H, true);
#if defined(PILOT_NET_L) && PILOT_NET_L >= 3
    // A third hidden layer, if the weights were fitted with one. Ping-ponging
    // between the two buffers rather than adding a third: only the last two
    // layers' outputs are ever live at once.
    layer(PILOT_W2, PILOT_B2, h1, h0, PILOT_NET_H, PILOT_NET_H, true);
    layer(PILOT_W3, PILOT_B3, h0, y,  PILOT_NET_H, PILOT_NET_OUT, false);
#else
    layer(PILOT_W2, PILOT_B2, h1, y,  PILOT_NET_H,  PILOT_NET_OUT, false);
#endif

    // The same squash the training used. Pitch and yaw move both ways from the
    // middle; the throttle does not.
    out->pitch    = fast_tanh(y[0]);
    out->yaw      = fast_tanh(y[1]);
    out->throttle = 0.5f * (fast_tanh(0.5f * y[2]) + 1.0f);   // = sigmoid(y)
#if PILOT_NET_OUT > 3
    const float pfire = 0.5f * (fast_tanh(0.5f * y[3]) + 1.0f);
    out->fire = (pfire >= PILOT_FIRE_T);
#else
    out->fire = false;
#endif
    // THE MODE, which is an argmax and needs no softmax to find.
    //
    // The trainer's last layer is a choice between the modes and it is trained
    // through a softmax, but softmax is monotonic: whichever score is largest
    // before it is largest after it. So the board skips the exponentials
    // entirely and reads the winner straight off the raw outputs.
    //
    // THE COUNT IS CHECKED, and this is the same lesson as the input width. A
    // mode added to vg_modes.h since these weights were fitted would be read off
    // an output that means something else, and it would look like a strategy
    // rather than like a bug.
    out->mode = -1;
#if defined(PILOT_MODE_N) && PILOT_MODE_N > 0
    if (PILOT_MODE_N == VG_MODE_N && PILOT_NET_OUT >= 4 + PILOT_MODE_N) {
        int best = 0;
        for (int i = 1; i < PILOT_MODE_N; i++)
            if (y[4 + i] > y[4 + best]) best = i;
        out->mode = best;
    }
#endif
    out->valid    = true;
}
