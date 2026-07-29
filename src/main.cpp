// PHANTOM - standalone entry point.
//
// This file exists only for the standalone build. When the game is embedded in
// another firmware, its launcher calls the same four functions per frame:
//   vg_input_update() -> vg_game_update() -> vg_render_frame() -> vg_rast_flush()

#include <Arduino.h>
#include "vg/vg_port.h"
#include "vg/vg_raster.h"
#include "vg/vg_input.h"
#include "vg/vg_game.h"
#include "vg/vg_render.h"
#include "vg/vg_capture.h"

// Set to 1 to stream raw accelerometer axes, for working out which way the
// board should tilt (see TILT_* in vg_config.h).
#define VG_DEBUG_TILT 0

static bool s_halted = false;

void setup(void) {
    Serial.begin(115200);
    delay(300);
    Serial.println("\n=== PHANTOM ===");

    if (!vg_panel_init()) {
        Serial.println("FATAL: no panel");
        s_halted = true;
        return;
    }
    if (!vg_rast_init()) {
        Serial.println("FATAL: rasteriser allocation failed");
        s_halted = true;
        return;
    }

    // Non-fatal: without an IMU you can still fly straight and shoot, and
    // without touch you can still watch the attract loop. Better to boot
    // degraded than to show a black screen.
    if (!vg_imu_init())   Serial.println("WARN: no IMU - tilt steering disabled");
    if (!vg_touch_init()) Serial.println("WARN: no touch - input disabled");
    // Also non-fatal: without storage the game forgets between power cycles,
    // which is worse than persisting and far better than refusing to boot.
    if (!vg_store_init()) Serial.println("WARN: no NVS - progress will not persist");
    vg_buttons_init();

    vg_input_init();
    vg_game_init();
    vg_input_calibrate();

    Serial.println("setup complete");
}

void loop(void) {
    if (s_halted) { delay(1000); return; }

    static uint32_t last_us   = 0;
    static float    fps       = 30.0f;
    static uint32_t report_ms = 0;
    static uint32_t acc_input = 0, acc_update = 0, acc_submit = 0, acc_flush = 0;
    static uint32_t acc_rast = 0, acc_wait = 0;
    static uint32_t acc_sky = 0, acc_prim = 0, acc_scan = 0;
    static uint32_t frames    = 0;

    vg_capture_poll();

    uint32_t now = micros();
    if (last_us == 0) last_us = now;
    float dt = (now - last_us) * 1e-6f;
    last_us = now;
    // A stalled frame must not teleport the world through an asteroid.
    if (dt < 0.0005f) dt = 0.0005f;
    if (dt > 0.10f)   dt = 0.10f;

    // While recording, the simulation is stepped at a FIXED rate no matter how
    // long the frame actually took. The device runs in slow motion because the
    // link cannot carry 460KB sixty times a second, but the recording plays
    // back perfectly smooth -- wall-clock speed decides how long you wait, not
    // how the video looks.
    const float cap_dt = vg_capture_dt();
    if (cap_dt > 0.0f) dt = cap_dt;

    uint32_t t0 = micros();
    VgInput in;
    vg_input_update(dt, &in);

    uint32_t t1 = micros();
    vg_game_update(dt, &in);

    uint32_t t2 = micros();
    vg_render_frame(&in, fps);

    uint32_t t3 = micros();
    vg_rast_flush();
    uint32_t t4 = micros();

    float inst = 1.0f / dt;
    fps += (inst - fps) * 0.08f;

    acc_input  += t1 - t0;
    acc_update += t2 - t1;
    acc_submit += t3 - t2;
    acc_flush  += t4 - t3;
    acc_rast   += vg_rast_raster_us();
    acc_wait   += vg_rast_wait_us();
    acc_sky    += vg_rast_sky_us();
    acc_prim   += vg_rast_prim_us();
    acc_scan   += vg_rast_scan_us();
    frames++;

    // Telemetry is suppressed while recording: it shares the link with the
    // frame stream, and a printf landing mid-band would corrupt the capture.
    uint32_t ms = millis();
    if (!vg_capture_active() && ms - report_ms >= 2000) {
        report_ms = ms;
        // rast is CPU spent building bands; wait is time stalled on the panel
        // DMA. Only the amount by which rast exceeds the transfer window costs
        // frame time, so those two numbers are what any optimisation is aimed at.
        Serial.printf("%.1f fps | in %lu upd %lu sub %lu blit %lu "
                      "| rast %lu = sky %lu prim %lu scan %lu "
                      "| P %d T %d%s\n",
                      (double)fps,
                      (unsigned long)(acc_input  / frames),
                      (unsigned long)(acc_update / frames),
                      (unsigned long)(acc_submit / frames),
                      (unsigned long)(acc_flush  / frames),
                      (unsigned long)(acc_rast   / frames),
                      (unsigned long)(acc_sky    / frames),
                      (unsigned long)(acc_prim   / frames),
                      (unsigned long)(acc_scan   / frames),
                      vg_rast_prim_count(),
                      vg_rast_tri_count(),
                      vg_rast_overflowed() ? " OVERFLOW" : "");
#if VG_DEBUG_TILT
        Serial.printf("   accel %.3f %.3f %.3f -> pitch %.2f yaw %.2f thr %.2f\n",
                      (double)in.raw_ax, (double)in.raw_ay, (double)in.raw_az,
                      (double)in.pitch, (double)in.yaw, (double)in.throttle);
#endif
        acc_input = acc_update = acc_submit = acc_flush = 0;
        acc_rast  = acc_wait = 0;
        acc_sky   = acc_prim = acc_scan = 0;
        frames = 0;
    }
}
