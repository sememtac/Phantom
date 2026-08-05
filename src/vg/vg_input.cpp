#include "vg_input.h"
#include "vg_config.h"
#include "vg_port.h"
#include "vg_capture.h"
#include <Arduino.h>
#include <math.h>

// Logs any single-frame throttle jump large enough to be physically impossible,
// with the raw contact list. Cheap, and it fires only on the fault.
#define VG_DEBUG_THROTTLE 1

// ---- throttle ----
static float s_throttle = 0.55f;
// The throttle is owned by a specific contact, tracked the same way the
// steering finger is. Without this, ANY point that happens to land in the left
// strip drives it -- including a spurious one -- and the setting jumps.
static bool  s_thr_active = false;
static float s_thr_x = 0, s_thr_y = 0;

// ---- steering ----
static bool  s_steer_active = false;
static float s_steer_ox = 0, s_steer_oy = 0;   // virtual-joystick origin
static float s_steer_x  = 0, s_steer_y  = 0;   // current position
static float s_steer_px = 0, s_steer_py = 0;   // previous position (trackball)
static float s_steer_age = 0;                  // seconds held
static float s_yaw = 0, s_pitch = 0;           // smoothed deflection
static bool  s_prev_steer_contact = false;
static bool  s_prev_fire_btn      = false;
static bool  s_prev_alt_btn       = false;
static bool  s_prev_touch         = false;
static float s_menu_x = 0, s_menu_y = 0;

// ---- tilt (STEER_MODE 2 only) ----
static float s_ax = 0, s_ay = 0, s_az = 1;
static float s_nx = 0, s_ny = 0, s_nz = 1;
static bool  s_have_sample = false;

void vg_input_init(void) {
    s_throttle = 0.55f;
    s_thr_active = false;
    s_steer_active = false;
    s_prev_steer_contact = false;
    s_yaw = s_pitch = 0;
    s_have_sample = false;
}

void vg_input_calibrate(void) {
#if STEER_MODE == 2
    // NOT WHILE THE LINK IS CARRYING FRAMES, for two reasons, and the second one
    // cost a fifteen-minute render.
    //
    // The IMU decides nothing during a replay: the recorded VgInput already holds
    // whatever attitude produced it. So twelve reads and 72ms of delay() were
    // being spent computing a neutral that nothing would ever read -- and
    // tools/README.md already claimed the device does not touch the IMU during a
    // render, which was simply not true here.
    //
    // The printf is the real damage. It lands BETWEEN TWO BANDS of a frame the
    // host is reading as pixels, and vg_capture.h states the rule plainly: one
    // stray print ends the recording. This is called from enter_course -- the
    // transition immediately after the ship is chosen -- so every render of a
    // session that passed through the course desynced at exactly that moment and
    // silently stopped writing video from there on.
    if (!vg_link_busy()) {
        float sx = 0, sy = 0, sz = 0;
        int   n  = 0;
        for (int i = 0; i < 12; i++) {
            float ax, ay, az;
            if (vg_imu_read(&ax, &ay, &az)) { sx += ax; sy += ay; sz += az; n++; }
            delay(6);
        }
        if (n > 0) {
            s_nx = sx / n; s_ny = sy / n; s_nz = sz / n;
            s_ax = s_nx;   s_ay = s_ny;   s_az = s_nz;
            s_have_sample = true;
        }
        Serial.printf("vg_input: neutral = %.3f %.3f %.3f (%d samples)\n",
                      s_nx, s_ny, s_nz, n);
    }
#endif
    s_yaw = s_pitch = 0;
    s_steer_active = false;
}

// ===========================================================================
// THE IMU AND THE PMU READ ON CORE 0. THE TOUCH CONTROLLER DOES NOT.
//
// Measured cause. The `in` phase billed 862-960 us a frame and almost none of it
// was arithmetic: it was I2C latency, for the touch controller, the IMU, and the
// PMU's power-key poll, all on the render thread in front of everything else.
//
// It matters because of WHICH budget it lands in. The panel costs 12.3 ms of every
// frame however little CPU there is -- 460,800 bytes over QSPI at 80 MHz, plus band
// zero, which cannot overlap anything -- so the work that bills frame time directly
// has 4.37 ms to fit in. Combat spends 5.9.
//
// WHAT MOVED AND WHAT DID NOT, because that distinction is the safety argument.
// Only READS moved, never a decision. Everything that decides anything stays on the
// render thread with the frame's own dt: partitioning contacts into throttle, rear
// and stick; the throttle's acquire-and-retain; the stick's origin, age and
// trackball delta; the button edges; the deflection smoothing. All of it is
// frame-coupled, and the VgInput it produces is what a `.phr` records -- so moving
// the state machine would change the feel AND the recording, where moving a bus wait
// changes neither.
//
// AND THE TOUCH READ CAME BACK. It went over too, on the first attempt, and broke
// steering -- see the note on Sensors below for the driver contract that forbids it.
// The frame is the only thing allowed to poll that part.
//
// REPLAY IS UNTOUCHED BY CONSTRUCTION, not by care: main.cpp calls vg_input_update
// only when the mode is not VG_RP_PLAY, so during a render this task is never even
// created. Verified against the d335430 baseline -- all nine frames identical.
//
// Two cores now share the Wire object, so every runtime I2C entry point in vg_port
// takes a mutex. That is NOT about the bus, which esp32-hal-i2c already serialises;
// it is about TwoWire's rxBuffer and rxIndex being members of one shared object, so
// two threads interleave their read-outs even when each transfer is atomic. See the
// note on the lock in vg_port_co5300.cpp.
//
// vg_input_calibrate keeps its own direct reads. They are safe under the same lock,
// and it is averaging twelve samples of the same sensor.
// ===========================================================================

// THE IMU ONLY. The touch controller CANNOT come over here, and the reason is in
// TouchClassCST226::getPoint: it returns 0 when the status buffer's fresh-data
// marker is absent, which is indistinguishable from "every finger lifted". The part
// reports at about 100 Hz, so reading once per 16-23 ms frame always finds a fresh
// sample -- but a task polling at 4 ms consumes it first, and the frame then sees
// no contacts on most frames.
//
// That was tried and it broke steering while leaving the throttle apparently fine,
// which is a diagnostic in itself: the throttle's retain guard holds its last value
// when no candidate contact appears, so it looks healthy, while the stick needs a
// live contact every frame and simply died. The consumer has to be the poller for
// this part, so the touch read stays on the render thread.
//
// The IMU has no such contract -- it is a continuous quantity, every read returns
// the current acceleration, and the value is smoothed against dt afterwards anyway.
// A sample up to 4 ms old is indistinguishable from a fresh one.
struct Sensors {
    float    ax, ay, az;
    bool     imu_ok;
};

// A SEQLOCK, not a double buffer. The task publishes every 4 ms against a 16-23 ms
// frame, so the writer laps the reader several times per read, and two alternating
// buffers would let the reader sit in the one the writer comes back to. The tear
// would be one axis of an acceleration beside another axis from a different sample
// -- a direction that was never measured, fed straight into the tilt steering.
//
// Odd while writing. Two stores for the writer, and a reader that checks the
// sequence is unchanged either side of its copy; in practice it never retries.
static Sensors  s_sens;
static uint32_t s_sens_seq = 0;

static void sensor_task(void*) {
    for (;;) {
        // The PMU comes with it. Off the render thread for the same reason the IMU
        // is -- three transactions and two clock changes -- and safe beside the
        // frame's touch read because every entry point in vg_port now takes the I2C
        // lock. Its own 50 ms gate is inside it, so calling it every pass is free.
        vg_pmu_poll();

        Sensors s;
        s.imu_ok = vg_imu_read(&s.ax, &s.ay, &s.az);

        const uint32_t q = __atomic_load_n(&s_sens_seq, __ATOMIC_RELAXED);
        __atomic_store_n(&s_sens_seq, q + 1, __ATOMIC_RELAXED);      // -> odd
        __atomic_thread_fence(__ATOMIC_RELEASE);
        s_sens = s;
        __atomic_thread_fence(__ATOMIC_RELEASE);
        __atomic_store_n(&s_sens_seq, q + 2, __ATOMIC_RELEASE);      // -> even

        // FASTER THAN A FRAME on purpose. The reads cost what they cost wherever
        // they run, so polling ahead of the frame means the sample a frame picks up
        // is at most this old -- the move buys input latency as well as frame time,
        // where leaving it in the frame tied freshness to the frame rate.
        vTaskDelay(pdMS_TO_TICKS(4));
    }
}

// The freshest complete sample. False if the task has not published one yet.
static bool sensors_take(Sensors* out) {
    for (int tries = 0; tries < 8; tries++) {
        const uint32_t a = __atomic_load_n(&s_sens_seq, __ATOMIC_ACQUIRE);
        if (a == 0 || (a & 1u)) continue;          // nothing yet, or mid-write
        *out = s_sens;
        __atomic_thread_fence(__ATOMIC_ACQUIRE);
        if (__atomic_load_n(&s_sens_seq, __ATOMIC_RELAXED) == a) return true;
    }
    return false;
}

// Deadzone, normalise to full deflection, then a mild expo so small movements
// give fine control while the outer travel still reaches full rate.
static inline float shape(float d, float dead, float full) {
    float a = fabsf(d);
    if (a <= dead) return 0.0f;
    float v = (a - dead) / (full - dead);
    if (v > 1.0f) v = 1.0f;
    v = v * v * 0.45f + v * 0.55f;
    return d > 0 ? v : -v;
}

void vg_input_update(float dt, VgInput* out) {
    // STARTED HERE, LAZILY, rather than in vg_input_init. The game is meant to drop
    // into another firmware whose launcher calls the four per-frame functions and
    // does its own init, so hanging the task off an init call makes input depend on
    // a host doing something in an order this file cannot check. First update is a
    // point every host must reach.
    static bool s_task = false;
    if (!s_task) {
        s_task = true;
        xTaskCreatePinnedToCore(sensor_task, "sens", 4096, nullptr, 2, nullptr, 0);
    }

    // ON THIS THREAD, always -- see the note on Sensors for why this one cannot
    // move. It is also the only I2C left in the frame.
    uint16_t xs[VG_MAX_TOUCH], ys[VG_MAX_TOUCH];
    const int n = vg_touch_read(xs, ys);

    Sensors sn;
    if (!sensors_take(&sn)) {
        // First frame after the task is created, before it has published anything.
        sn.imu_ok = vg_imu_read(&sn.ax, &sn.ay, &sn.az);
    }

    // ---- partition contacts into throttle side and steering side ----
    int zone[VG_MAX_TOUCH];
    int nzone = 0;
    int other[VG_MAX_TOUCH];
    int nother = 0;
    // Three buckets, not two. The rear-view patch is held down like a button,
    // and a thumb resting on it must not also be flying the ship -- the whole
    // point of looking behind is to hold heading while you do it.
    //
    // THE STEERING FINGER KEEPS THE STEERING FINGER'S JOB. A swipe that runs up
    // into the top right is still a swipe: the patch used to claim any contact
    // inside its zone, so carrying a turn that far dropped the contact out of
    // the steering set -- which both threw the view aft and let go of the stick,
    // in the middle of the turn that got there.
    //
    // So the patch only takes a contact that ARRIVED in it. The test is the same
    // one the throttle uses to keep hold of its own thumb: a real finger moves
    // tens of pixels a frame, not hundreds, so anything close to where the stick
    // was last frame IS the stick, wherever it has got to.
    bool rear = false;
    for (int i = 0; i < n; i++) {
        if (xs[i] <= THROTTLE_ZONE_X1) {
            if (nzone < VG_MAX_TOUCH) zone[nzone++] = i;
            continue;
        }

        const bool in_rear = (xs[i] >= REAR_ZONE_X0 && xs[i] <= REAR_ZONE_X1 &&
                              ys[i] >= REAR_ZONE_Y0 && ys[i] <= REAR_ZONE_Y1);
        if (in_rear) {
            bool is_stick = false;
            if (s_steer_active) {
                const float dx = (float)xs[i] - s_steer_x;
                const float dy = (float)ys[i] - s_steer_y;
                is_stick = (dx * dx + dy * dy) <=
                           (THROTTLE_MAX_JUMP * THROTTLE_MAX_JUMP);
            }
            if (!is_stick) { rear = true; continue; }
        }

        if (nother < VG_MAX_TOUCH) other[nother++] = i;
    }
    out->rear_held = rear;

    // ---- throttle ----
    // Ownership with two guards. Nearest-match alone is NOT enough: when the
    // real thumb lifts, a spurious contact appearing in the same frame becomes
    // the only candidate in the zone and inherits the control, which is exactly
    // how the throttle used to snap to zero on release.
    int thr_i = -1;

    if (s_thr_active) {
        // Retain: nearest candidate, but only if it could plausibly BE our
        // thumb one frame later.
        float best = 1e30f;
        int   cand = -1;
        for (int k = 0; k < nzone; k++) {
            float dx = (float)xs[zone[k]] - s_thr_x;
            float dy = (float)ys[zone[k]] - s_thr_y;
            float d2 = dx * dx + dy * dy;
            if (d2 < best) { best = d2; cand = zone[k]; }
        }
        if (cand >= 0 && best <= THROTTLE_MAX_JUMP * THROTTLE_MAX_JUMP) thr_i = cand;
        else s_thr_active = false;
    }

    bool fresh_grab = false;
    if (!s_thr_active) {
        // Acquire: must land ON the slider, not merely somewhere down the left
        // edge. A contact that fails this leaves the throttle exactly as it was.
        for (int k = 0; k < nzone; k++) {
            uint16_t y = ys[zone[k]];
            if (y >= THROTTLE_ZONE_Y0 && y <= THROTTLE_ZONE_Y1) {
                thr_i = zone[k];
                s_thr_active = true;
                fresh_grab   = true;
                break;
            }
        }
    }

    float prev_throttle = s_throttle;

    if (thr_i >= 0) {
        s_thr_x = (float)xs[thr_i];
        s_thr_y = (float)ys[thr_i];
        float t = ((float)THROTTLE_BOT - s_thr_y) /
                  (float)(THROTTLE_BOT - THROTTLE_TOP);
        if (t < 0) t = 0; else if (t > 1) t = 1;
        // Snap to the thumb. A slider that lags the finger feels broken; the
        // smoothing that matters happens in the flight model instead.
        s_throttle = t;
    }

#if VG_DEBUG_THROTTLE
    // Only an anomaly if the value jumped while a contact was ALREADY holding
    // the throttle -- a thumb cannot drag that far in one frame. A fresh grab
    // legitimately snaps to wherever it landed, so it is not reported.
    if (!fresh_grab && fabsf(s_throttle - prev_throttle) > 0.35f && !vg_link_busy()) {
        Serial.printf("THR JUMP %.2f->%.2f | n=%d thr_i=%d nzone=%d |",
                      (double)prev_throttle, (double)s_throttle, n, thr_i, nzone);
        for (int i = 0; i < n; i++) Serial.printf(" (%u,%u)", xs[i], ys[i]);
        Serial.println();
    }
#else
    (void)prev_throttle;
#endif

    // ---- pick the steering contact ----
    // The controller does not give stable IDs across reads, so track the
    // steering finger as whichever current contact is nearest to where it was.
    int steer_i = -1;
    if (nother > 0) {
        if (s_steer_active) {
            float best = 1e30f;
            for (int k = 0; k < nother; k++) {
                float dx = (float)xs[other[k]] - s_steer_x;
                float dy = (float)ys[other[k]] - s_steer_y;
                float d2 = dx * dx + dy * dy;
                if (d2 < best) { best = d2; steer_i = other[k]; }
            }
        } else {
            steer_i = other[0];
            s_steer_active = true;
            s_steer_ox = s_steer_x = s_steer_px = (float)xs[steer_i];
            s_steer_oy = s_steer_y = s_steer_py = (float)ys[steer_i];
            s_steer_age = 0;
        }
    }

    // Firing moved to a hardware button, so a contact on the steering side is
    // now unambiguously steering -- no tap-versus-drag discrimination, and no
    // reason to hold still for fear of loosing a missile.
    if (steer_i >= 0) {
        s_steer_px = s_steer_x;
        s_steer_py = s_steer_y;
        s_steer_x  = (float)xs[steer_i];
        s_steer_y  = (float)ys[steer_i];
        s_steer_age += dt;
    } else if (s_steer_active) {
        s_steer_active = false;
    }

    // ---- steering ----
    float target_yaw = 0, target_pitch = 0;

#if STEER_MODE == 0
    if (steer_i >= 0) {
        float dx = s_steer_x - s_steer_ox;
        float dy = s_steer_y - s_steer_oy;
#if STEER_RECENTER
        float len = sqrtf(dx * dx + dy * dy);
        if (len > STEER_RANGE) {
            float k = (len - STEER_RANGE) / len;
            s_steer_ox += dx * k;
            s_steer_oy += dy * k;
            dx = s_steer_x - s_steer_ox;
            dy = s_steer_y - s_steer_oy;
        }
#endif
        // Pointer-style, like an FPS look control: the nose goes where the
        // finger goes. Drag toward the top-right and the ship aims up-right.
        // (Screen y grows downward, so dy is used unnegated here.)
        target_yaw   = shape(dx, STEER_DEADZONE, STEER_RANGE);
        target_pitch = shape(dy, STEER_DEADZONE, STEER_RANGE) * STEER_PITCH_SIGN;
    }

#elif STEER_MODE == 1
    if (steer_i >= 0 && s_steer_age > dt * 0.5f) {
        // The game applies yaw_in * TURN_RATE * dt, so undo that here to make
        // the turn proportional to distance travelled rather than to time.
        float inv = 1.0f / (TURN_RATE * (dt > 1e-4f ? dt : 1e-4f));
        target_yaw   = (s_steer_x - s_steer_px) * TRACKBALL_RAD_PER_PX * inv;
        target_pitch = (s_steer_y - s_steer_py) * TRACKBALL_RAD_PER_PX * inv
                       * STEER_PITCH_SIGN;
        if (target_yaw   >  1) target_yaw   =  1;
        if (target_yaw   < -1) target_yaw   = -1;
        if (target_pitch >  1) target_pitch =  1;
        if (target_pitch < -1) target_pitch = -1;
    }

#else  // STEER_MODE == 2, tilt
    if (sn.imu_ok) {
        const float ax = sn.ax, ay = sn.ay, az = sn.az;
        if (!s_have_sample) { s_ax = ax; s_ay = ay; s_az = az; s_have_sample = true; }
        else {
            float k = dt * TILT_LERP;
            if (k > 1.0f) k = 1.0f;
            s_ax += (ax - s_ax) * k;
            s_ay += (ay - s_ay) * k;
            s_az += (az - s_az) * k;
        }
    }
    {
        float dx = s_ax - s_nx;
        float dy = s_ay - s_ny;
#if TILT_SWAP_AXES
        target_yaw   = shape(dy, TILT_DEADZONE, TILT_FULL) * TILT_YAW_SIGN;
        target_pitch = shape(dx, TILT_DEADZONE, TILT_FULL) * TILT_PITCH_SIGN;
#else
        target_yaw   = shape(dx, TILT_DEADZONE, TILT_FULL) * TILT_YAW_SIGN;
        target_pitch = shape(dy, TILT_DEADZONE, TILT_FULL) * TILT_PITCH_SIGN;
#endif
    }
#endif

    float k = dt * STEER_LERP;
    if (k > 1.0f) k = 1.0f;
    s_yaw   += (target_yaw   - s_yaw)   * k;
    s_pitch += (target_pitch - s_pitch) * k;

    bool steer_contact = (nother > 0);

    out->pitch     = s_pitch;
    out->yaw       = s_yaw;
    out->throttle  = s_throttle;

    // Launch on the press EDGE, not while held: one press, one missile. Holding
    // would empty the rack in three seconds, which is the opposite of the
    // deliberate, committed shot the lock mechanic is built around. No debounce
    // needed -- the class's own fire_gap already swallows any contact chatter.
    const uint8_t btns = vg_buttons_read();

    const bool fire_btn = (btns & FIRE_BUTTON_MASK) != 0;
    out->fire_btn  = fire_btn;
    out->fire_edge = fire_btn && !s_prev_fire_btn;
    s_prev_fire_btn = fire_btn;

    const bool alt_btn = (btns & ALT_BUTTON_MASK) != 0;
    out->alt_edge = alt_btn && !s_prev_alt_btn;
    s_prev_alt_btn = alt_btn;

    // Already an edge, and already latched: the PMU reports a short press as a
    // discrete event, so there is no held state to difference against.
    out->pwr_edge = vg_pmu_pwr_pressed();

    // Roll takes over the HORIZONTAL axis. PITCH KEEPS WORKING.
    //
    // It used to be zeroed too, on the reasoning that rolling and pitching from
    // one contact was two commands from one gesture. That is exactly backwards
    // for a flying machine: rolling to set the plane of a turn and then pulling
    // through it IS the manoeuvre, and forbidding the pull left the roll as a
    // rotation of the view with nothing on the other end of it -- the ship
    // rolled, and then waited to be told what the roll had been for.
    //
    // It reuses the shaped, smoothed yaw axis rather than reading the raw finger,
    // so roll gets the same deadzone and the same ramp as every other control and
    // does not need its own feel.
    // AND STEERING IS HANDED BACK GRADUALLY, not in one frame.
    //
    // yaw is zeroed while the button is held, so releasing it with the finger still deflected
    // returned full deflection instantly -- a violent yaw the moment a roll ended, which is
    // exactly when a pilot is least expecting the ship to bite. The finger has not moved; only
    // the meaning of where it is has changed, and a change of meaning should not read as a
    // change of command.
    //
    // Only yaw needs it. Pitch is live throughout a roll by design -- see the note above -- so
    // it never steps, and gating it would take away the pull-through that the roll exists to
    // set up.
    static float s_yaw_gate = 1.0f;
    out->roll_btn = alt_btn;
    if (alt_btn) {
        out->roll  = out->yaw;
        out->yaw   = 0.0f;
        s_yaw_gate = 0.0f;
    } else {
        out->roll = 0.0f;
        if (s_yaw_gate < 1.0f) {
            s_yaw_gate += ROLL_HANDBACK;
            if (s_yaw_gate > 1.0f) s_yaw_gate = 1.0f;
            out->yaw *= s_yaw_gate;
        }
    }

    out->tap_edge  = steer_contact && !s_prev_steer_contact;
    out->any_touch = (n > 0);

    // ---- menu pointer ----
    // Contact 0 rather than the steering contact, so a menu button under the
    // left edge is still reachable. Delta is zeroed on the press frame: the
    // previous position belongs to a lift that may have been anywhere.
    const bool touched = (n > 0);
    const float mx = touched ? (float)xs[0] : s_menu_x;
    const float my = touched ? (float)ys[0] : s_menu_y;

    out->menu_edge = touched && !s_prev_touch;
    out->menu_held = touched;
    out->menu_x    = mx;
    out->menu_y    = my;
    out->menu_dx   = (touched && s_prev_touch) ? (mx - s_menu_x) : 0.0f;
    out->menu_dy   = (touched && s_prev_touch) ? (my - s_menu_y) : 0.0f;

    s_menu_x     = mx;
    s_menu_y     = my;
    s_prev_touch = touched;

    out->steering  = s_steer_active;
    out->steer_ox  = s_steer_ox;
    out->steer_oy  = s_steer_oy;
    out->steer_x   = s_steer_x;
    out->steer_y   = s_steer_y;

    out->raw_ax = s_ax;
    out->raw_ay = s_ay;
    out->raw_az = s_az;

    s_prev_steer_contact = steer_contact;
}
