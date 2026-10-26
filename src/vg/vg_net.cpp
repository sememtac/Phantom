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

#if !(defined(PILOT_NET_INT8) && PILOT_NET_INT8)   // the float network's pieces
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
#endif

#if (defined(PILOT_NET_INT8) && PILOT_NET_INT8) || defined(_MSC_VER)
// One int8 layer's sums: acc = W * in + b, all integer. Row by row, as the float
// layer. Used by the compiled int8 network and by a loaded one alike -- so it is
// built where either of them is.
static void qlayer(const int8_t* w, const int32_t* b, const int8_t* in, int32_t* acc,
                   int n_in, int n_out) {
    for (int j = 0; j < n_out; j++) {
        const int8_t* row = w + (size_t)j * n_in;
        int32_t a = b[j];
        for (int i = 0; i < n_in; i++) a += (int32_t)row[i] * (int32_t)in[i];
        acc[j] = a;
    }
}
#endif

#if defined(PILOT_NET_INT8) && PILOT_NET_INT8
// ---- THE INT8 PIECES. See tools/quantize_pilot.py, which is their reference. ----
//
// WHY INT8, MEASURED: the float network cost about 1 ms a call in a live fight on
// the board against 233 us warm, because its 28 KB of weights live in flash and the
// canopy pushes them out of the shared data cache every frame. The int8 arrays are
// DRAM_ATTR, in internal RAM, so the cache never sees them -- 4 KB for a 32-wide
// network -- and an int8 multiply is cheaper than a float one besides.

// A sum to a tanh-table index: round(acc * m / 2^SHIFT), held inside the table. The
// product needs 64 bits. The shift of a negative number rounds down, the same as
// the reference's.
static inline int q_index(int32_t acc, int32_t m) {
    int64_t v = ((int64_t)acc * (int64_t)m + ((int64_t)1 << (PILOT_Q_SHIFT - 1)))
                >> PILOT_Q_SHIFT;
    if (v < -PILOT_Q_LUT_HALF) v = -PILOT_Q_LUT_HALF;
    if (v >  PILOT_Q_LUT_HALF) v =  PILOT_Q_LUT_HALF;
    return (int)v + PILOT_Q_LUT_HALF;
}

// The activation: each sum through its own scale into the tanh table.
static void qact(const int32_t* acc, const int32_t* m, int8_t* out, int n) {
    for (int j = 0; j < n; j++) out[j] = PILOT_Q_TANH[q_index(acc[j], m[j])];
}
#endif

bool vg_net_available(void) { return true; }

// ---- A LOADED INT8 NETWORK. See VgNetQ in vg_net.h. ------------------------
//
// The compiled int8 network's arithmetic, step for step, over a struct instead of
// the header's macros. Kept apart from the compiled code on purpose: the compiled
// path is what the board runs and has been checked bit for bit, and a loaded one
// only ever runs on the desktop. --net-check holds this one to the same reference.
//
// DESKTOP ONLY, and compiled out of the firmware the same way vg_raster.cpp keeps
// its overflow report out: the board never loads a network from a file, so code
// that cannot run there is not carried there.
#if defined(_MSC_VER)
#include "vg_bot.h"   // VG_OBS_N, for the bound below

static const VgNetQ* s_use[2] = { nullptr, nullptr };   // [0] opponents, [1] the seat

void vg_net_use(int seat, const VgNetQ* q) {
    if (seat == 0 || seat == 1) s_use[seat] = q;
}

static inline int gq_index(const VgNetQ* q, int32_t acc, int32_t m) {
    int64_t v = ((int64_t)acc * (int64_t)m + ((int64_t)1 << (q->shift - 1))) >> q->shift;
    if (v < -q->lut_half) v = -q->lut_half;
    if (v >  q->lut_half) v =  q->lut_half;
    return (int)v + q->lut_half;
}

// The first ship within the tolerance on every field, or -1: the gate's rule.
static int gq_ship_row(const VgNetQ* q, const float* obs, int n) {
    const float* mine = obs + (n - q->ship_n);
    for (int k = 0; k < q->n_ships; k++) {
        const float* row = q->ships + (size_t)k * q->ship_n;
        bool same = true;
        for (int i = 0; i < q->ship_n && same; i++) {
            float d = mine[i] - row[i];
            if (d < 0.0f) d = -d;
            if (d > q->ship_tol) same = false;
        }
        if (same) return k;
    }
    return -1;
}

static bool gq_knows(const VgNetQ* q, const float* obs, int n) {
    if (!obs || n < q->n_in) return false;
    if (q->n_ships <= 0) return true;
    return gq_ship_row(q, obs, n) >= 0;
}

// The stack arrays below are VG_NET_Q_MAX long and the loader holds n_in to the
// observation's width, so this is what keeps x[] in bounds. Checked here rather than
// trusted, because the observation is the thing that grows.
static_assert(VG_OBS_N <= VG_NET_Q_MAX, "the observation is wider than a loaded network may be");

static void gq_run(const VgNetQ* q, const float* obs, int n, VgNetOut* out) {
    if (n != q->n_in) { out->valid = false; return; }
    int8_t  x[VG_NET_Q_MAX], ha[VG_NET_Q_MAX], hb[VG_NET_Q_MAX];
    int32_t acc[VG_NET_Q_MAX];
    for (int i = 0; i < q->n_in; i++) {
        const int32_t xi = (int32_t)(obs[i] * (float)q->in_fix);
        // In 64 bits: in_mean comes from a file, and an int32 difference of two
        // values from different places could overflow. The same numbers as the
        // compiled path whenever it does not.
        int64_t v = ((int64_t)xi - (int64_t)q->in_mean[i]) * (int64_t)q->in_mul[i];
        v = (v + ((int64_t)1 << (q->in_shift - 1))) >> q->in_shift;
        if (v < -127) v = -127;
        if (v >  127) v =  127;
        x[i] = (int8_t)v;
    }
    const int8_t* in = x;
    int in_n = q->n_in;
    for (int l = 0; l < q->n_l; l++) {
        int8_t* h = (l & 1) ? hb : ha;
        qlayer(q->w[l], q->b[l], in, acc, in_n, q->nh);
        for (int j = 0; j < q->nh; j++) h[j] = q->lut[gq_index(q, acc[j], q->m[l][j])];
        in = h;
        in_n = q->nh;
    }
    qlayer(q->w[q->n_l], q->b[q->n_l], in, acc, in_n, q->n_out);
    const int32_t* m_out = q->m[q->n_l];

    const float k127 = 1.0f / 127.0f;
    out->pitch    = (float)q->lut[gq_index(q, acc[0], m_out[0])] * k127;
    out->yaw      = (float)q->lut[gq_index(q, acc[1], m_out[1])] * k127;
    out->throttle = 0.5f * ((float)q->lut[gq_index(q, acc[2], q->m_half)] * k127 + 1.0f);
    int32_t cut = q->fire_acc;
    if (q->fire_acc_ship) {
        const int k = gq_ship_row(q, obs, n);
        if (k >= 0) cut = q->fire_acc_ship[k];
    }
    out->fire = (acc[3] >= cut);
    out->mode = -1;
    if (q->m_mode && q->n_mode == VG_MODE_N && q->n_out >= 4 + q->n_mode) {
        int best = 0;
        int64_t bs = (int64_t)acc[4] * (int64_t)q->m_mode[0];
        for (int i = 1; i < q->n_mode; i++) {
            const int64_t s = (int64_t)acc[4 + i] * (int64_t)q->m_mode[i];
            if (s > bs) { bs = s; best = i; }
        }
        out->mode = best;
    }
    out->valid = true;
}
#endif   // _MSC_VER: the loaded network

// ---- THE COMPILED NETWORK ---------------------------------------------------

static bool compiled_knows_ship(const float* obs, int n) {
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

#if defined(PILOT_FIRE_PER_SHIP) && PILOT_FIRE_PER_SHIP && defined(PILOT_SHIPS) && PILOT_SHIPS > 0
// WHICH SHIP THIS IS, as a row of PILOT_SHIP_SEEN: the first within the tolerance
// on every field, which is the gate's own rule, or -1. It chooses the trigger cut.
// One cut for all four ships made the LANCE network almost never fire and the
// CHARIOT fire too often; see PILOT_FIRE_T_SHIP in the header.
static int ship_row(const float* obs, int n) {
    const float* mine = obs + (n - PILOT_SHIP_N);
    for (int k = 0; k < PILOT_SHIPS; k++) {
        const float* row = PILOT_SHIP_SEEN + (size_t)k * PILOT_SHIP_N;
        bool same = true;
        for (int i = 0; i < PILOT_SHIP_N && same; i++) {
            float d = mine[i] - row[i];
            if (d < 0.0f) d = -d;
            if (d > PILOT_SHIP_TOL) same = false;
        }
        if (same) return k;
    }
    return -1;
}
#endif

static void compiled_run(const float* obs, int n, VgNetOut* out) {
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

#if defined(PILOT_NET_INT8) && PILOT_NET_INT8
    // ---- THE INT8 NETWORK ------------------------------------------------
    //
    // Integer from the observation to the outputs, and it has to be: the same
    // arithmetic runs in tools/quantize_pilot.py, and the PC's --net-check holds
    // the two to the same bits. A float anywhere in the middle would let the
    // compiler fuse or reorder it differently on each platform.
    //
    // THE INPUT FIRST BECOMES AN INTEGER BY A POWER OF TWO, which is exact in
    // float and truncates the same way everywhere. Everything after it is integer.
    int8_t x[PILOT_NET_IN];
    for (int i = 0; i < PILOT_NET_IN; i++) {
        const int32_t xi = (int32_t)(obs[i] * (float)PILOT_Q_IN_FIX);
        int64_t v = (int64_t)(xi - PILOT_Q_IN_MEAN[i]) * (int64_t)PILOT_Q_IN_MUL[i];
        v = (v + ((int64_t)1 << (PILOT_Q_IN_SHIFT - 1))) >> PILOT_Q_IN_SHIFT;
        if (v < -127) v = -127;
        if (v >  127) v =  127;
        x[i] = (int8_t)v;
    }

    int8_t  qh0[PILOT_NET_H];
    int8_t  qh1[PILOT_NET_H];
    int32_t acc[PILOT_NET_H > PILOT_NET_OUT ? PILOT_NET_H : PILOT_NET_OUT];
    qlayer(PILOT_QW0, PILOT_QB0, x,   acc, PILOT_NET_IN, PILOT_NET_H);
    qact(acc, PILOT_QM0, qh0, PILOT_NET_H);
    qlayer(PILOT_QW1, PILOT_QB1, qh0, acc, PILOT_NET_H, PILOT_NET_H);
    qact(acc, PILOT_QM1, qh1, PILOT_NET_H);
#  if defined(PILOT_NET_L) && PILOT_NET_L >= 3
    qlayer(PILOT_QW2, PILOT_QB2, qh1, acc, PILOT_NET_H, PILOT_NET_H);
    qact(acc, PILOT_QM2, qh0, PILOT_NET_H);
    qlayer(PILOT_QW3, PILOT_QB3, qh0, acc, PILOT_NET_H, PILOT_NET_OUT);
    const int32_t* m_out = PILOT_QM3;
#  else
    qlayer(PILOT_QW2, PILOT_QB2, qh1, acc, PILOT_NET_H, PILOT_NET_OUT);
    const int32_t* m_out = PILOT_QM2;
#  endif

    // The same squashes as the float network, read off the same table: pitch and
    // yaw are tanh, the throttle is tanh of half the value. The conversion to
    // float is one multiply by a constant, which rounds the same everywhere.
    const float k127 = 1.0f / 127.0f;
    out->pitch    = (float)PILOT_Q_TANH[q_index(acc[0], m_out[0])] * k127;
    out->yaw      = (float)PILOT_Q_TANH[q_index(acc[1], m_out[1])] * k127;
    out->throttle = 0.5f * ((float)PILOT_Q_TANH[q_index(acc[2], PILOT_Q_M_HALF)] * k127 + 1.0f);
    // The trigger cut, moved onto the sum: sigmoid(z) >= T is z >= logit(T).
    int32_t cut = PILOT_Q_FIRE_ACC;
#  if defined(PILOT_FIRE_PER_SHIP) && PILOT_FIRE_PER_SHIP && defined(PILOT_SHIPS) && PILOT_SHIPS > 0
    {
        const int k = ship_row(obs, n);
        if (k >= 0) cut = PILOT_Q_FIRE_ACC_SHIP[k];
    }
#  endif
    out->fire = (acc[3] >= cut);
    out->mode = -1;
#  if defined(PILOT_MODE_N) && PILOT_MODE_N > 0
    if (PILOT_MODE_N == VG_MODE_N && PILOT_NET_OUT >= 4 + PILOT_MODE_N) {
        // Each sum has its own scale, so the comparison is of sum * scale. The
        // first of two equal scores wins, as in the reference.
        int best = 0;
        int64_t bs = (int64_t)acc[4] * (int64_t)PILOT_Q_M_MODE[0];
        for (int i = 1; i < PILOT_MODE_N; i++) {
            const int64_t s = (int64_t)acc[4 + i] * (int64_t)PILOT_Q_M_MODE[i];
            if (s > bs) { bs = s; best = i; }
        }
        out->mode = best;
    }
#  endif
    out->valid = true;
#else
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
    float cut = PILOT_FIRE_T;
#  if defined(PILOT_FIRE_PER_SHIP) && PILOT_FIRE_PER_SHIP && defined(PILOT_SHIPS) && PILOT_SHIPS > 0
    {
        const int k = ship_row(obs, n);
        if (k >= 0) cut = PILOT_FIRE_T_SHIP[k];
    }
#  endif
    out->fire = (pfire >= cut);
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
#endif   // PILOT_NET_INT8
}

// ---- WHICH NETWORK FLIES ----------------------------------------------------
//
// A loaded network for that seat if the desktop gave one, and the compiled one
// otherwise. The seat never borrows the opponents' loaded network: in self-play
// the two sides are two different networks, and silently sharing one would make
// every such measurement a mirror match.

bool vg_net_knows_ship(const float* obs, int n) {
#if defined(_MSC_VER)
    if (s_use[0]) return gq_knows(s_use[0], obs, n);
#endif
    return compiled_knows_ship(obs, n);
}

void vg_net_run(const float* obs, int n, VgNetOut* out) {
    if (!obs || !out) return;
#if defined(_MSC_VER)
    if (s_use[0]) { gq_run(s_use[0], obs, n, out); return; }
#endif
    compiled_run(obs, n, out);
}

bool vg_net_knows_ship_seat(const float* obs, int n) {
#if defined(_MSC_VER)
    if (s_use[1]) return gq_knows(s_use[1], obs, n);
#endif
    return compiled_knows_ship(obs, n);
}

void vg_net_run_seat(const float* obs, int n, VgNetOut* out) {
    if (!obs || !out) return;
#if defined(_MSC_VER)
    if (s_use[1]) { gq_run(s_use[1], obs, n, out); return; }
#endif
    compiled_run(obs, n, out);
}
