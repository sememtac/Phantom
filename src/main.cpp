// PHANTOM - standalone entry point.
//
// This file exists only for the standalone build. When the game is embedded in
// another firmware, its launcher calls the same four functions per frame:
//   vg_input_update() -> vg_game_update() -> vg_render_frame() -> vg_rast_flush()

#include <Arduino.h>
#include <esp_system.h>
#include <esp_heap_caps.h>
#include "vg/vg_port.h"
#include "vg/vg_raster.h"
#include "vg/vg_input.h"
#include "vg/vg_game.h"
#include "vg/vg_render.h"
#include "vg/vg_capture.h"
#include "vg/vg_replay.h"
#include "vg/vg_crumb.h"
#include "vg/vg_sfx.h"

// Set to 1 to stream raw accelerometer axes, for working out which way the
// board should tilt (see TILT_* in vg_config.h).
#define VG_DEBUG_TILT 0

static bool s_halted = false;

void setup(void) {
    // Frame capture pushes tens of KB per frame and the default TX ring is a
    // few hundred bytes, so the writer stalled packet by packet and capture ran
    // at 5 fps. With room to queue it runs at 22 -- a 4.3x gain from one line,
    // and by far the largest single win in this feature.
    //
    // 16K is the knee. Measured: 4K ring 5.1 fps, 16K 21.98 fps, 32K 22.22 fps,
    // at which point 0.74 MB/s is the USB-Serial-JTAG peripheral's own ceiling
    // and more buffer buys nothing but RAM.
    Serial.setTxBufferSize(16384);
    Serial.begin(115200);

    // NEVER BLOCK THE GAME ON THE SERIAL PORT. Zero means write() drops whatever
    // does not fit and returns immediately.
    //
    // This was 5000, so that a capture could not lose a frame when the host
    // paused. It also meant that with nobody reading the port -- which is every
    // normal session -- the 16KB ring slowly filled with telemetry, and then
    // every two-second printf blocked for FIVE SECONDS. The game froze, ran for
    // two seconds, and froze again, a few minutes into play.
    //
    // The long timeout is correct only while a capture is running, and only then
    // is a host actually reading. vg_capture_set() raises it and lowers it again.
    Serial.setTxTimeoutMs(0);

    // Room for more than one replay record in flight, so the host can keep the
    // next frame queued while the device is still sending the current one.
    Serial.setRxBufferSize(2048);

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

    // Why the LAST run ended, and where it was. A panic on this part with USB-CDC
    // prints nothing -- the USB task dies with it -- so a crash is silent and
    // indistinguishable from a hang. This is the only thing that will tell us.
    //
    //   1 power-on   3 software   4 PANIC   5 int watchdog   6 task watchdog
    //   7 watchdog   9 brownout  11 USB, which is our own reset pulse
    //
    // AFTER storage, not before. The record is mirrored into flash so that
    // unplugging a locked-up board does not erase the reason it locked up, and
    // reporting it before the store exists would quietly skip exactly the case it
    // was added for.
    vg_crumb_report();

    vg_buttons_init();
    // Non-fatal like the rest: without the PMU the power key is invisible and
    // every other control still works.
    if (!vg_pmu_init()) Serial.println("WARN: no PMU - power key unavailable");
    // Non-fatal like the rest. A silent game is a lesser game; a game that
    // refuses to boot because a codec did not answer is a broken one.
    if (!vg_sfx_init()) Serial.println("WARN: no audio");

#if VG_AUDIO_CHIRP
    // TEMP: the two that changed. Off, on, then three hull hits -- three, because
    // the growl is a 13 Hz judder and one of them is over before the ear has
    // decided what it heard.
    {
        const SfxId demo[5] = { SFX_TV_OFF, SFX_TV_ON, SFX_HIT, SFX_HIT, SFX_HIT };
        const int   hold[5] = { 55, 60, 110, 110, 130 };
        for (int i = 0; i < 5; i++) {
            vg_sfx_play(demo[i], 1.0f);
            for (int k = 0; k < hold[i]; k++) { vg_sfx_update(); delay(10); }
        }
    }
#endif
    vg_pmu_dump();

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

    // Not while replaying: the host is sending frame records, and the capture
    // poller would eat them as if they were commands.
    vg_crumb(CRUMB_POLL, (uint8_t)vg.state);
    if (vg_replay_mode() != VG_RP_PLAY) vg_capture_poll();

    uint32_t now = micros();
    if (last_us == 0) last_us = now;
    float frame_dt = (now - last_us) * 1e-6f;
    last_us = now;
    if (frame_dt < 0.0005f) frame_dt = 0.0005f;

    // Before the clamp below throws the evidence away. A quarter of a second is
    // fifteen frames' worth: far past a hitch, and the only trace a freeze that
    // never resets will ever leave.
    //
    // The PHASE is worked out at the end of the frame, where the timings already
    // exist -- see below. Recorded here it would always say "flush", because that
    // is simply the last crumb the previous frame happened to set.
    //
    // NOT DURING A SESSION. A capture or a replay blocks the loop on purpose --
    // the device waits up to thirty seconds for the host's next record -- and
    // those waits are both longer and far more frequent than any real freeze, so
    // left in they would own the worst-frame record permanently and the
    // instrument would only ever report the tool measuring it.
    const bool stalled = (frame_dt > 0.25f)
                       && !vg_capture_active() && vg_replay_mode() == VG_RP_OFF;
    const uint32_t stall_ms = (uint32_t)(frame_dt * 1000.0f);
    // Past half a second the frame is not late, something has blocked -- a flash
    // write, a reconnect. Catching up on it is worse than dropping it.
    if (frame_dt > 0.50f) frame_dt = 0.50f;

    // A long frame is SUB-STEPPED rather than clamped.
    //
    // Clamping was fine while a long frame meant a hitch, and wrong once a
    // frame could take 180ms: every one would have been clamped to 100ms and
    // the world would have advanced at half wall-clock speed.
    //
    // Sub-stepping keeps real time AND keeps the guarantee the clamp existed for
    // -- no single step long enough to put a missile through a hull. Normal play
    // is a 16ms frame and one step, exactly as before.
    float sim_dt = frame_dt;

    uint32_t t0 = micros();
    vg_crumb(CRUMB_INPUT, (uint8_t)vg.state);
    VgInput in;

    // Replaying: the frame's length and every input come off the wire, and the
    // hardware is never asked. Feeding the recorded VgInput straight in is what
    // makes touch and IMU irrelevant to reproducing a session -- whatever they
    // produced is already in the struct.
    if (vg_replay_mode() == VG_RP_PLAY) {
        if (!vg_replay_next(&sim_dt, &in)) return;
    } else {
        vg_input_update(sim_dt, &in);
    }

    // Sub-step long frames, as above. A replayed frame is already 1/60s, so
    // this costs it nothing.
    int steps = 1;
    float dt = sim_dt;
    if (dt > 0.02f) {
        steps = (int)(dt / 0.02f) + 1;
        dt    = sim_dt / (float)steps;
    }

    uint32_t t1 = micros();
    vg_crumb(CRUMB_UPDATE, (uint8_t)vg.state);
    for (int s = 0; s < steps; s++) vg_game_update(dt, &in);

    // Straight after the step, so seeds drawn during it belong to this frame.
    vg_replay_note_frame(sim_dt, &in);

    uint32_t t2 = micros();
    vg_crumb(CRUMB_RENDER, (uint8_t)vg.state);
    vg_render_frame(&in, fps);

    // Generated after the frame is submitted and before the blit waits on DMA,
    // which is the one place in the loop with time to spare.
    vg_sfx_update();

    uint32_t t3 = micros();
    vg_crumb(CRUMB_FLUSH, (uint8_t)vg.state);
    vg_rast_flush();
    uint32_t t4 = micros();

    // The rate the FRAME went out at, not the sub-step rate -- sub-steps are an
    // implementation detail of a long frame and would read as a speed-up.
    float inst = 1.0f / sim_dt;   // NOLINT: sim_dt is never zero, clamped above
    fps += (inst - fps) * 0.08f;

    // Which quarter of the frame actually ate the time. The gap is measured
    // between loop entries, so the offender is in the frame that just finished --
    // and if none of the four phases accounts for it, the time went somewhere
    // outside them, which means the capture poll or the telemetry write.
    if (stalled) {
        const uint32_t d_in  = t1 - t0, d_upd = t2 - t1;
        const uint32_t d_ren = t3 - t2, d_fls = t4 - t3;
        uint32_t worst = d_in; uint8_t phase = CRUMB_INPUT;
        if (d_upd > worst) { worst = d_upd; phase = CRUMB_UPDATE; }
        if (d_ren > worst) { worst = d_ren; phase = CRUMB_RENDER; }
        if (d_fls > worst) { worst = d_fls; phase = CRUMB_FLUSH;  }
        if (worst < 200000u) phase = CRUMB_POLL;      // none of them: outside
        vg_crumb_stall(stall_ms, (uint8_t)vg.state, phase);
    }

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
    // Also silent while recording a session: the log shares this link, and a
    // telemetry line landing between frame records is indistinguishable from a
    // corrupt one to the host.
    // availableForWrite() as well: with the timeout at zero a full ring only
    // costs a dropped line, but checking first means the frame does not even pay
    // for formatting one nobody can receive.
    if (!vg_capture_active() && vg_replay_mode() == VG_RP_OFF
        && ms - report_ms >= 2000 && Serial.availableForWrite() > 256) {
        report_ms = ms;
        uint8_t pmu_seen[3];
        vg_pmu_seen(pmu_seen);
        // rast is CPU spent building bands; wait is time stalled on the panel
        // DMA. Only the amount by which rast exceeds the transfer window costs
        // frame time, so those two numbers are what any optimisation is aimed at.
        Serial.printf("%.1f fps | in %lu upd %lu sub %lu blit %lu "
                      "| rast %lu = sky %lu prim %lu scan %lu "
                      "| P %d T %d | heap %luK stack %luB | pmu %02X%02X%02X%s\n",
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
                      // A leak shows as heap falling steadily; a stack overflow
                      // shows as headroom approaching zero before it panics.
                      // Both were invisible until now.
                      (unsigned long)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024),
                      (unsigned long)(uxTaskGetStackHighWaterMark(NULL)),
                      // Every AXP2101 interrupt bit seen since boot. The power
                      // key is in here somewhere; one press names it.
                      pmu_seen[0], pmu_seen[1], pmu_seen[2],
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
