// PHANTOM - standalone entry point.
//
// This file exists only for the standalone build. When the game is embedded in
// another firmware, its launcher calls the same four functions per frame:
//   vg_input_update() -> vg_game_update() -> vg_render_frame() -> vg_rast_flush()

#include <Arduino.h>
#include "esp_freertos_hooks.h"
#include "esp_task_wdt.h"

uint32_t vg_render_mirror_us(void);   // diagnostic, defined in vg_render.cpp
int      vg_fire_live(void);          // live fireballs, defined in vg_game.cpp
// THE PROFILING COUNTERS ARE NOT DEFINED HERE ANY MORE. Each is defined by the
// module that writes it, which is what the other nine already did -- g_hud_radar
// in vg_hud.cpp, g_synth_peak in vg_synth.cpp, g_upd_* in vg_game.cpp.
//
// They lived here, and this file exists only for the standalone build. Every one
// is written unconditionally from src/vg/, so a launcher that dropped main.cpp
// took 21 undefined symbols with it and the four-function contract at the top of
// this file did not hold. Nothing was failing -- there is no embedded build yet --
// but the promise was not one this file could keep.
//
// This one stays: it is main's own accumulator, not a counter anyone else writes.
static uint32_t g_sfx_us;   // the synth, also inside the submit phase
#include <esp_system.h>
#include <esp_heap_caps.h>
#include "vg/vg_port.h"
#include "vg/vg_sky.h"
#include "vg/vg_raster.h"
#include "vg/vg_canopy_draw.h"
#include "vg/vg_input.h"
#include "vg/vg_game.h"
#include "vg/vg_render.h"
#include "vg/vg_prof.h"
#include "vg/vg_capture.h"
#include "vg/vg_replay.h"
#include "vg/vg_crumb.h"
#include "vg/vg_sfx.h"
#include "vg/vg_synth.h"

// Set to 1 to stream raw accelerometer axes, for working out which way the
// board should tilt (see TILT_* in vg_config.h).
#define VG_DEBUG_TILT 0

static bool s_halted = false;

// HOW IDLE CORE 0 ACTUALLY IS, measured rather than inferred from a table of task
// costs. The idle hook runs each pass of core 0's idle task; two hook entries close
// together mean the core did nothing between them, so summing only the short deltas
// counts idle time and excludes the work that ran between distant ones. The threshold
// is generous against the pass cost and tiny against any real job.
//
// This exists because every plan to offload work to core 0 rests on how much idle it
// has, and that number had only ever been arithmetic. It reports through the replay's
// BLIT line as idle0.
// COUNTING PASSES, NOT CYCLES, because the idle task on this RTOS does not spin: it
// executes waiti and sleeps to the next 1 ms tick. A cycle-delta scheme read every gap
// as work and reported zero. With waiti, one hook entry is one tick the core spent
// asleep-idle, so the pass count IS idle milliseconds -- coarse, and decisive enough
// against a question asked in whole milliseconds.
static volatile uint32_t g_idle0_passes = 0;
static bool idle0_hook(void) {
    // Read-then-write rather than ++, which C++20 deprecates on a volatile: the
    // standard stopped guaranteeing how many accesses the compound form implies.
    // Identical here -- one hook entry, one increment, and the only other toucher
    // is the telemetry window, which reads and clears it once a frame.
    g_idle0_passes = g_idle0_passes + 1;
    return true;   // no extra sleep; the idle task carries on to its own waiti
}

void setup(void) {
    esp_register_freertos_idle_hook_for_cpu(idle0_hook, 0);
    // PROBE DISABLED: the pilot said the steering was wrong, and that verdict outranks
    // every counter. The touch read needs ~600 us of contiguous time and core 0's idle
    // comes in ~130 us slivers, so the task's samples arrive stretched and irregular.
    // Left in the tree, not started.
    // vg_input_probe_start();
    // Frame capture pushes tens of KB per frame and the default TX ring is a
    // few hundred bytes, so the writer stalled packet by packet and capture ran
    // at 5 fps. With room to queue it runs at 22 -- a 4.3x gain from one line,
    // and by far the largest single win in this feature.
    //
    // 16K is the knee. Measured: 4K ring 5.1 fps, 16K 21.98 fps, 32K 22.22 fps,
    // at which point 0.74 MB/s is the USB-Serial-JTAG peripheral's own ceiling
    // and more buffer buys nothing but RAM.
    // The core's own log output goes nowhere while a session owns the port. Installed here,
    // before anything else can log: an I2S error burst at boot used to land inside the frame
    // stream and end a recording after one frame. See vg_link_guard_logs.
    vg_link_guard_logs();

    // A HANG THAT DOES NOT RESET LEAVES NO EVIDENCE. The crumb records the
    // frame and the phase into RTC memory every frame, but it is only promoted
    // to a crash record on an abnormal reset -- so a freeze that merely stops
    // drawing, which is what has been reported twice, throws that away.
    //
    // The watchdog turns one into the other. Ten seconds is far above anything
    // legitimate: a frame is 16ms and the longest single call in the game is
    // vg_sky_generate at 146ms. The two places that legitimately wait longer
    // are both link waits, and both feed the dog themselves.
    {
        esp_task_wdt_config_t wdt = { };
        wdt.timeout_ms     = 10000;
        wdt.idle_core_mask = 0;
        wdt.trigger_panic  = true;
        if (esp_task_wdt_init(&wdt) == ESP_ERR_INVALID_STATE)
            esp_task_wdt_reconfigure(&wdt);   // the core got there first
        esp_task_wdt_add(NULL);               // watch the loop task
    }

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
    // ONE, NOT ZERO, AND ZERO IS THE BUG. Read HWCDC::write before changing this.
    //
    // The driver's inner loop keeps `uint32_t tries = tx_timeout_ms` as its escape from a
    // ring that is not draining: every iteration that makes no progress does `tries--`
    // and `delay(1)`, and at `tries == 0` it declares the host gone and gives up. Setting
    // the timeout to zero starts that counter AT zero, so the first no-progress iteration
    // underflows it to 4,294,967,295 and the escape can never fire. The write then spins
    // on delay(1) for about seven weeks.
    //
    // Which is what a task watchdog at ten seconds calls a reset. It needs the ring FULL
    // and the port enumerated but unread -- a board plugged into a PC with no terminal
    // open, which is every normal session -- and the 16 KB ring takes about forty seconds
    // of two-second telemetry to fill. Frames 2410, 2691, 2696, 2700, 2701 and 2714
    // across six crashes, all in that window, and never once while a tool held the port,
    // because a tool reading the port is a tool draining the ring.
    //
    // Zero was put here to fix the opposite fault -- a 5000 ms timeout froze the game for
    // five seconds at a time once the ring filled -- and the note above is still right
    // about that. It just picked the one value that turns a five-second freeze into a
    // permanent one. One millisecond is the smallest value that still counts down.
    Serial.setTxTimeoutMs(1);

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
    // THE BACKDROP CLAIMS ITS TEXTURE FIRST, and the order is the whole point.
    //
    // Three things need INTERNAL SRAM and cannot use PSRAM: the primitive list, the band
    // buffers, and the backdrop's 32 KB texture. All three are read in scattered patterns
    // every frame, which is the finding the two-stage rasteriser is built on.
    //
    // The backdrop used to allocate last, from vg_game_init, and a third band buffer took
    // the room it needed -- 41 KB free but the largest contiguous block only 21 KB, so a
    // 32 KB request failed and the nebula switched off entirely. Total free said there was
    // room; contiguous free said there was not.
    //
    // Asking first is what makes that unrepresentable rather than a sum to keep re-checking:
    // the one allocation with a hard contiguous minimum takes it from an unfragmented heap,
    // and the renderer sizes itself against what is left. vg_sky_init is idempotent, so
    // vg_game_init's own call still finds the texture and does the rest of its work.
    if (!vg_sky_init()) Serial.println("WARN: no backdrop - out of internal SRAM");

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
    vg_pmu_dump();

    vg_input_init();
    vg_game_init();
    vg_input_calibrate();

    Serial.println("setup complete");
}

void loop(void) {
    esp_task_wdt_reset();
    if (s_halted) { delay(1000); return; }

    static uint32_t last_us   = 0;
    static float    fps       = 30.0f;
    static uint32_t report_ms = 0;
    static uint32_t acc_input = 0, acc_update = 0, acc_submit = 0, acc_flush = 0;
    static uint32_t acc_rast = 0, acc_wait = 0, acc_push = 0;
    static uint32_t acc_over_us = 0, acc_over_n = 0;
    static uint32_t acc_join = 0, acc_res = 0;
    static uint32_t acc_can = 0;
    static uint32_t acc_canh = 0, acc_canw = 0;
    static uint32_t acc_idle0 = 0;
    static uint32_t acc_join_mm = 0, acc_join_n = 0;
    static uint32_t acc_sky = 0, acc_prim = 0, acc_scan = 0;
    static uint32_t acc_aa = 0, acc_ln = 0, acc_tri2 = 0, acc_oth = 0;
    // `oth` split three ways, because it is a bucket and each part moves for its own reason
    // -- points with speed, glyphs with what the HUD is saying, fills with the instruments on
    // screen. Optimising a bucket is how four rounds of canopy work came out unattributable.
    static uint32_t acc_pt = 0, acc_gl = 0, acc_fl = 0;
    static uint32_t acc_lnpx = 0, acc_lnn = 0;
    static uint32_t acc_tint = 0, acc_mir = 0;
    static uint32_t frames    = 0;

    // THE DISTRIBUTION, not just the mean.
    //
    // The mean read 58.1 fps while the steady state was 59.6, because a handful
    // of frames were dragging it. An average cannot tell those apart, and they
    // are opposite problems: being 0.5 ms short on EVERY frame is a budget to
    // trim, and being 8 ms short once a second is an event to find. Optimising
    // for the wrong one is how you spend a week making the fast frames faster.
    //
    // A histogram rather than a running percentile: no sorting, no allocation,
    // integer bucket arithmetic, 66 bytes. 0.5 ms buckets from 12 ms, which
    // brackets comfortably-fast through twice the budget, and the top bucket
    // catches everything worse.
#define FT_BUCKETS 33
#define FT_BASE_US 12000u
#define FT_STEP_US 500u
    static uint16_t ft_hist[FT_BUCKETS];
    // And the worst frame of the window WITH its phase split, because "p95 is
    // 24 ms" is half an answer -- the other half is which phase owned it, and
    // that is the half that says what to go and look at.
    static uint32_t ft_worst = 0;
    static uint32_t ft_w_in = 0, ft_w_upd = 0, ft_w_sub = 0, ft_w_blit = 0;
    static uint8_t  ft_w_state = 0;
    // Counted exactly rather than read off the histogram. 16667 us falls INSIDE
    // a 0.5 ms bucket (bucket 9 spans 16.5-17.0), so a bucket-derived count
    // would silently miss every frame between 16.67 and 17.0 ms -- and this is
    // the number anybody reads first. One compare is cheaper than the caveat.
    static uint32_t ft_late = 0;
    // AND THE REPORT FRAME DOES NOT COUNT.
    //
    // The three telemetry lines are ~600 characters into the serial ring, and
    // they cost about 1.8 ms on the frame that writes them -- which lands in the
    // NEXT frame's period, because the period is measured at the top of the loop.
    // Left in, that frame won the "worst frame" slot almost every window, and its
    // phase split summed to 1.8 ms less than its period, because the cost is
    // outside t0..t4 entirely. The first reading off this instrument was the
    // instrument.
    //
    // Same rule the stall record already applies to capture waits, for the same
    // reason: never let the measurement be the thing measured.
    static bool ft_skip_next = false;

    // Not while replaying: the host is sending frame records, and the capture
    // poller would eat them as if they were commands.
    vg_crumb(CRUMB_POLL, (uint8_t)vg.state);
    if (vg_replay_mode() != VG_RP_PLAY) vg_capture_poll();

    uint32_t now = micros();
    if (last_us == 0) last_us = now;
    // Kept as an integer, before every clamp below touches it. This is the real
    // frame PERIOD -- what the panel actually showed -- rather than the sim step,
    // which is sub-stepped and clamped and therefore lies about long frames on
    // purpose.
    const uint32_t frame_us = now - last_us;
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
    // THE FPS READOUT MUST NOT CARRY HISTORY INTO A REPLAY.
    //
    // `fps` is a smoothed average, and it is DRAWN. Before a render starts the
    // device has been sitting at the menu for however long the host took to get
    // organised, so the average has converged on the menu's rate -- and that
    // rate differs a little from boot to boot. The replay then continues
    // smoothing from that value, so the number on screen depends on what
    // happened BEFORE the recording began.
    //
    // Rendered twice, one frame differed by 94 pixels out of 230,400, all of
    // them inside the one 32-row band that holds this counter. That was enough
    // to make every frame hash differ, which reads as a simulation that is not
    // reproducible -- when the simulation was fine and a debug overlay was not.
    //
    // Reset on the edge into PLAY, so the number becomes a pure function of the
    // recorded frame times. It still shows what the rate was, which is the point
    // of it; it just no longer remembers anything from before.
    static int prev_rp = VG_RP_OFF;
    const int rp_now = vg_replay_mode();
    if (rp_now == VG_RP_PLAY && prev_rp != VG_RP_PLAY) fps = 30.0f;
    prev_rp = rp_now;

    if (rp_now == VG_RP_PLAY) {
        if (!vg_replay_next(&sim_dt, &in)) return;
    } else {
        // PROBE: the update lives on a core-0 task; take false means it never started
        // and the inline path is exactly what it always was.
        if (!vg_input_take(&in)) vg_input_update(sim_dt, &in);
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

    // AN EDGE IS CONSUMED ONCE, not once per sub-step.
    //
    // The same VgInput used to be handed to every sub-step, so on any frame long
    // enough to be split, every press happened as many times as there were
    // steps. For a toggle that is catastrophic and silent: PWR paused on the
    // first step and un-paused on the second, so an even split left the player
    // exactly where they started and the key looked dead.
    //
    // Which is why it looked like the BOUNDARY ALARM was blocking pause. It was
    // not. Being near a wall means the screen tint is running, the tint costs
    // about four milliseconds, four milliseconds is what pushes a 16ms frame past
    // the 20ms sub-step threshold -- and from there the alarm and the broken
    // pause key share a cause without one causing the other.
    //
    // Firing, tapping and menu presses were all doubling too, on exactly the
    // frames where the game was already struggling.
    VgInput sub = in;
    for (int s = 0; s < steps; s++) {
        vg_game_update(dt, &sub);
        if (s == 0) {
            sub.fire_edge = sub.alt_edge = false;
            sub.menu_edge = sub.pwr_edge = false;
        }
    }

    // Straight after the step, so seeds drawn during it belong to this frame.
    vg_replay_note_frame(sim_dt, &in);

    uint32_t t2 = micros();
    // SNAPSHOT THE UPDATE'S SPANS HERE, because the telemetry block below zeroes them
    // every frame and the replay's own report runs after it -- read there, all eleven
    // come back 0 while `upd` reads a quarter of a second. Taken at the end of the
    // update, which is the only moment they are both complete and still alive.
    {
        const uint32_t u[11] = { g_upd_pre, g_upd_ship, g_upd_arena, g_upd_sky,
                                 g_upd_field, g_upd_trail, g_upd_enemy, g_upd_ord,
                                 g_upd_vfx, g_upd_ai, g_upd_combat };
        for (int i = 0; i < 11; i++) g_upd_snap[i] = u[i];
    }
    vg_crumb(CRUMB_RENDER, (uint8_t)vg.state);
    vg_render_frame(&in, fps);

    // Generated after the frame is submitted and before the blit waits on DMA,
    // which is the one place in the loop with time to spare.
    {
        // The synth bills to the submit phase and is invisible in the layer
        // timers -- it was the unaccounted milliseconds. Worse, it renders
        // wall-clock samples: a slow frame owes MORE audio, so the cost rises
        // exactly when the budget is shortest.
        const uint32_t t_sfx = micros();
        vg_sfx_update(sim_dt);
        g_sfx_us = micros() - t_sfx;
    }

    uint32_t t3 = micros();
    vg_crumb(CRUMB_FLUSH, (uint8_t)vg.state);
    vg_rast_flush();
    // THE CANOPY'S WARP MAPS, REBUILT HERE AND NOT IN SUBMIT. Every band has been drawn
    // by now, so nothing reads the maps until the next frame; the last bands are still
    // going out, so this runs against the wire instead of in front of it. It was inside
    // group B, where it spiked to 1,175 us on a throttle step and made core 1 wait.
    { const uint32_t t_w = micros(); vg_canopy_warp_build(); g_sub_warp = micros() - t_w; }
    uint32_t t4 = micros();
    // Everything from here to the dog feed at the top of the next loop() used to be
    // reported as "flush-push, band 14". See CRUMB_FEND.
    vg_crumb(CRUMB_TAIL, (uint8_t)vg.state);

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

    // NOT DURING A SESSION, for the same reason the stall record is not: a
    // capture or a replay blocks the loop waiting on the host, and those waits
    // are longer and far more frequent than any real slow frame. Left in, they
    // would own every percentile and the instrument would only ever be measuring
    // the tool measuring it.
    if (ft_skip_next) {
        ft_skip_next = false;               // this frame carries the printf
    } else if (!vg_capture_active() && vg_replay_mode() == VG_RP_OFF) {
        uint32_t fb = (frame_us > FT_BASE_US)
                    ? (frame_us - FT_BASE_US) / FT_STEP_US : 0u;
        if (fb >= FT_BUCKETS) fb = FT_BUCKETS - 1;
        ft_hist[fb]++;
        if (frame_us > 16667u) ft_late++;
        if (frame_us > ft_worst) {
            ft_worst   = frame_us;
            ft_w_in    = t1 - t0;
            ft_w_upd   = t2 - t1;
            ft_w_sub   = t3 - t2;
            ft_w_blit  = t4 - t3;
            ft_w_state = (uint8_t)vg.state;
        }
    }

    acc_input  += t1 - t0;
    acc_update += t2 - t1;
    acc_submit += t3 - t2;
    acc_flush  += t4 - t3;
    acc_rast   += vg_rast_raster_us();
    acc_wait   += vg_rast_wait_us();
    acc_push   += vg_rast_push_us();
    acc_over_us += vg_rast_over_us();
    acc_over_n  += (uint32_t)vg_rast_over_bands();
    acc_join    += vg_rast_join_us();
    acc_res     += vg_rast_res_us();
    acc_join_mm += vg_rast_join_mm_us();
    acc_join_n  += (uint32_t)vg_rast_join_n();
    static uint32_t acc_band[NUM_BANDS];
    {
        const uint32_t* bu = vg_rast_band_us();
        for (int i = 0; i < NUM_BANDS; i++) acc_band[i] += bu[i];
    }
    acc_sky    += vg_rast_sky_us();
    acc_aa     += vg_rast_aa_us();
    acc_ln     += vg_rast_ln_us();
    acc_tri2   += vg_rast_tri_us();
    acc_oth    += vg_rast_oth_us();
    acc_lnpx   += vg_rast_ln_px();
    acc_lnn    += vg_rast_ln_n();
    acc_pt     += vg_rast_pt_us();
    acc_gl     += vg_rast_gl_us();
    acc_fl     += vg_rast_fl_us();
    acc_can    += vg_rast_can_us();
    acc_canh   += vg_rast_canhalf_us();
    acc_canw   += vg_rast_canwait_us();
    // AND THE SPLIT MOVES ITSELF. Read here because this is where the frame's counters are
    // read once; the nudge is a row at a time, so it settles over a second or so and cannot
    // chase a single heavy frame.
    vg_canopy_split_nudge(vg_rast_canhalf_us(), vg_rast_canwait_us());
    // NOT MICROSECONDS. `tnt` is the wall warning's LEVEL, 0 to 100 -- its cost lives in
    // `sky` and cannot be separated from the fill it colours. See vg_rast_tint_us.
    acc_tint   += vg_rast_tint_us();
    acc_mir    += vg_render_mirror_us();
    static uint32_t acc_star = 0, acc_aren = 0, acc_wrld = 0, acc_hud = 0;
    static uint32_t acc_sfx = 0, acc_sxr = 0;
    acc_sfx += g_sfx_us;
    acc_sxr += g_sfx_render_us;
    acc_star += g_sub_star; acc_aren += g_sub_arena;
    acc_wrld += g_sub_world; acc_hud += g_sub_hud;
    static uint32_t acc_touch = 0, acc_lock = 0;
    acc_touch += g_in_touch; g_in_touch = 0;
    acc_lock  += g_in_lock;  g_in_lock  = 0;
    static uint32_t acc_suba = 0, acc_subb = 0;
    acc_suba += g_sub_a; acc_subb += g_sub_b;
    static uint32_t acc_ahoop = 0, acc_arail = 0;
    acc_ahoop += g_arena_hoop; g_arena_hoop = 0;
    acc_arail += g_arena_rail; g_arena_rail = 0;
    // The update's nine spans. Reset here, accumulated here: they are written with += in
    // the simulation so a frame split into sub-steps reports the FRAME's cost and not the
    // last step's. See g_upd_pre in vg_prof.h.
    static uint32_t acc_u[11] = {0};
    {
        uint32_t* const g[11] = { &g_upd_pre, &g_upd_ship, &g_upd_arena, &g_upd_sky,
                                  &g_upd_field, &g_upd_trail, &g_upd_enemy, &g_upd_ord,
                                  &g_upd_vfx, &g_upd_ai, &g_upd_combat };
        for (int i = 0; i < 11; i++) { acc_u[i] += *g[i]; *g[i] = 0; }
    }
    // Group B's own parts. Reset here with everything else in the window.
    static uint32_t acc_slock = 0, acc_scan_p = 0, acc_smarks = 0, acc_sover = 0;
    acc_slock  += g_sub_lock;   g_sub_lock   = 0;
    acc_scan_p += g_sub_canopy; g_sub_canopy = 0;
    acc_smarks += g_sub_marks;  g_sub_marks  = 0;
    acc_sover  += g_sub_over;   g_sub_over   = 0;
    // `world`'s five parts. See g_w_motes in vg_prof.h -- read the RANGE of these,
    // not the mean: the question is which one grows when the fight gets busy.
    static uint32_t acc_w[6] = {0};
    {
        uint32_t* const g[6] = { &g_w_motes, &g_w_rocks, &g_w_trails,
                                 &g_w_ships, &g_w_msl, &g_w_fire };
        // THE TIMED REPLAY GETS THEM HERE, before the zeroing on the same line.
        //
        // It used to read them thirty lines further down, after this block had already
        // cleared them for the next frame -- so a full session's world split came back as
        // five zeros, from counters that were working perfectly. The order was the whole
        // bug, and a zero is the one value that looks like "nothing happened" rather than
        // like a fault.
        vg_replay_note_world(g_w_motes, g_w_rocks, g_w_trails, g_w_ships,
                             g_w_msl, g_w_fire, g_sub_world);
        for (int i = 0; i < 6; i++) { acc_w[i] += *g[i]; *g[i] = 0; }
    }
    static uint32_t acc_hud_radar = 0, acc_hud_thr = 0;
    acc_hud_radar += g_hud_radar;  g_hud_radar    = 0;
    acc_hud_thr   += g_hud_throttle; g_hud_throttle = 0;
    // THE TIMED REPLAY'S SAMPLE, taken here because every counter it wants has just been
    // read for the window above and reading them twice would be reading them at two
    // different moments.
    //
    // Only the CPU-work counters go over. Frame time and blit deliberately do not: with no
    // pixels being streamed the panel is never the bottleneck, so those measure a pipeline
    // the player never runs. What is left is the work itself, which is the same work whether
    // or not anybody is watching the pixels. See vg_replay_timed.
    // Core 0's idle, drained EVERY frame -- the first version drained it inside the
    // timed-replay branch below, which measures the replay's own idle and answers
    // nothing about flight. One pass is one tick the idle task slept through, so the
    // count reads as milliseconds; short wakes overcount, which makes it an upper
    // bound and a fragmentation signal at once.
    const uint32_t g_i0_frame = g_idle0_passes; g_idle0_passes = 0;
    acc_idle0 += g_i0_frame;

    if (vg_replay_timed()) {
        vg_replay_note_cost(vg_rast_can_us(), vg_rast_raster_us(), vg_rast_prim_us(),
                            t3 - t2, t2 - t1, vg_rast_scan_us(), vg_rast_tv_us());
        // AND WHAT THE BLIT WAS DOING, from the same read of the same counters. `push` is
        // the CPU stopped against a full SPI queue and `over` is the part of the raster
        // that outran its band's window -- between them they say whether the wire is
        // waiting on the CPU or the CPU on the wire.
        vg_replay_note_blit(vg_rast_join_us(), vg_rast_wait_us(), vg_rast_push_us(),
                            vg_rast_res_us(), (uint32_t)vg_rast_over_bands(),
                            vg_rast_over_us(), vg_rast_sky_us(), vg_rast_scan_us());
        vg_replay_note_bands(vg_rast_band_us(), NUM_BANDS);
        vg_replay_note_cache();
        vg_replay_note_idle0(g_i0_frame * 1000u);   // one pass = one asleep tick = ~1 ms
        vg_replay_note_sub(g_sub_a, g_sub_b, g_sub_wait,
                           g_sub_arena, g_sub_star, g_sub_hud);
        vg_replay_note_types(vg_rast_aa_us(), vg_rast_ln_us(), vg_rast_tri_us(),
                             vg_rast_gl_us(), vg_rast_fl_us());
        // AND YIELD, because nothing else in this mode does.
        //
        // Every other way of running a frame blocks somewhere: gameplay waits on the panel,
        // a streamed replay waits on 460 KB going out over the link. A timed replay waits
        // on neither -- records are already in the host's buffer and no pixels are sent --
        // so the loop task spins, the idle task on this core never runs, and the watchdog
        // resets the board. That looks like nothing at all from the host, because the panic
        // goes to UART0 while this link is USB CDC: the port simply stops answering.
        //
        // One tick, once a frame. It costs a millisecond of wall clock that these counters
        // do not measure anyway -- they are CPU spent on work already done by this point.
        vTaskDelay(1);

        // ONE BYTE BACK, WHICH IS THE FLOW CONTROL.
        //
        // A streamed replay paces itself: the host sends the next record only after it has
        // read a whole frame, so it is never more than two ahead. A timed replay sends no
        // frame, so without this the host writes the entire session at once -- 81 KB for a
        // short one -- the device's CDC ring overflows, and the record stream desyncs.
        //
        // What that looked like was worse than a dropped byte. The device fell out of the
        // replay mid-record, the command poller resumed on the REMAINDER of a record, and
        // an 0x52 inside somebody's input blob is the letter R: it started a recording and
        // streamed PHRC entries at the host until it was reset.
        //
        // So the device says it has finished a frame and the host keeps a small window of
        // records outstanding. One byte a frame against 460,800 that a streamed replay
        // sends, and the run still goes at whatever rate the device can draw.
        // FLUSHED, or it is not an acknowledgement. USB CDC holds a lone byte until it has
        // a packet's worth or a timer expires, so unflushed acks arrive in late bursts --
        // the host then spends its time in the timeout path and a 900-frame run took 385
        // seconds instead of fifteen. One USB transaction a frame is the price of the
        // pacing working at all.
        Serial.write('.');
        Serial.flush();
    }
    acc_prim   += vg_rast_prim_us();
    acc_scan   += vg_rast_scan_us();
    frames++;

    // Telemetry is suppressed while recording: it shares the link with the
    // frame stream, and a printf landing mid-band would corrupt the capture.
    uint32_t ms = millis();
    // Also silent while recording a session: the log shares this link, and a
    // telemetry line landing between frame records is indistinguishable from a
    // corrupt one to the host.
    // The WINDOW closes on time whether or not the report goes out. Only the
    // printing is conditional on the link having room -- the accumulators reset
    // either way, because when they reset inside the print block a skipped line
    // rolled its whole window into the next one: the next report then averaged
    // over four seconds and `frames` became a running total. One run read 35637
    // frames and 625% blocked time, which is not a number, it is two windows
    // stacked. Per-window values are only sound if the window is really a window.
    //
    // availableForWrite() rather than letting the write block: with the timeout
    // at zero a full ring costs a dropped line, but checking first means the
    // frame does not even pay for formatting one nobody can receive.
    const bool win = !vg_capture_active() && vg_replay_mode() == VG_RP_OFF
                   && ms - report_ms >= 2000;
    // ROOM FOR THE WHOLE REPORT, not for a token 256 bytes of it. This block writes
    // about a kilobyte across eight calls, so a 256-byte check passed and then handed the
    // rest to the driver's full-ring path -- the one that had to be escaped from. Asking
    // for the whole thing means a full ring costs a dropped line here and nothing at all
    // in the driver.
    if (win && Serial.availableForWrite() > 2048) {
        // ON ITS OWN CRUMB. The guard checks for 256 bytes once and then writes about a
        // kilobyte, so it does not prove the write cannot block -- it only proves the
        // ring was not completely full when we asked.
        vg_crumb(CRUMB_TELEM, (uint8_t)vg.state);
        uint8_t pmu_seen[3];
        vg_pmu_seen(pmu_seen);
        // blit is now fully accounted: wait is the previous frame's last transfer
        // draining, rast is CPU spent building bands, push is the CPU standing
        // still because the panel had not finished the band before. The three sum
        // to blit by construction, and WHICH of them dominates decides what is
        // worth optimising -- push says the wire gates the frame, over says the
        // raster is spilling out of the transfer window.
        Serial.printf("%.1f fps | in %lu upd %lu sub %lu "
                      "| blit %lu = wait %lu rast %lu push %lu join %lu(mm %lu n %lu) res %lu "
                      "| sky %lu prim %lu scan %lu | over %lu.%lu/%d by %lu "
                      "| aa %lu ln %lu(%lupx %lun) tri %lu pt %lu gl %lu fl %lu oth %lu can %lu tnt %lu mir %lu "
                      "| canh %lu canw %lu cana %d i0 %lu "
                      "| P %d/%d T %d | heap %luK stack %luB | pmu %02X%02X%02X%s\n",
                      (double)fps,
                      (unsigned long)(acc_input  / frames),
                      (unsigned long)(acc_update / frames),
                      (unsigned long)(acc_submit / frames),
                      (unsigned long)(acc_flush  / frames),
                      (unsigned long)(acc_wait   / frames),
                      (unsigned long)(acc_rast   / frames),
                      (unsigned long)(acc_push   / frames),
                      (unsigned long)(acc_join   / frames),
                      (unsigned long)(acc_join_mm / frames),
                      (unsigned long)(acc_join_n  / frames),
                      (unsigned long)(acc_res    / frames),
                      (unsigned long)(acc_sky    / frames),
                      (unsigned long)(acc_prim   / frames),
                      (unsigned long)(acc_scan   / frames),
                      // Bands over the window, to a tenth: an integer mean would
                      // read as zero for the case that matters, one heavy band in
                      // every few frames.
                      (unsigned long)(acc_over_n * 10 / frames / 10),
                      (unsigned long)(acc_over_n * 10 / frames % 10),
                      (int)NUM_BANDS,
                      (unsigned long)(acc_over_us / frames),
                      (unsigned long)(acc_aa   / frames),
                      (unsigned long)(acc_ln   / frames),
                      (unsigned long)(acc_lnpx / frames),
                      (unsigned long)(acc_lnn  / frames),
                      (unsigned long)(acc_tri2 / frames),
                      (unsigned long)(acc_pt   / frames),
                      (unsigned long)(acc_gl   / frames),
                      (unsigned long)(acc_fl   / frames),
                      (unsigned long)(acc_oth  / frames),
                      (unsigned long)(acc_can  / frames),
                      (unsigned long)(acc_tint / frames),
                      (unsigned long)(acc_mir  / frames),
                      (unsigned long)(acc_canh / frames),
                      (unsigned long)(acc_canw / frames),
                      vg_rast_can_split(),
                      (unsigned long)(acc_idle0 * 1000u / frames),
                      vg_rast_prim_count(),
                      vg_rast_prim_peak(),
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
        // A second line rather than a longer one: the first is already at the
        // edge of what a terminal shows without wrapping. It shares the guard
        // and the window of the first, so the two lines average the same frames.
        // `hoops` is the label the split era left behind: the rails moved to core 0
        // and back again -- see the note at the submit call in vg_render.cpp -- so
        // this figure is the whole grid now, and the grid line below still splits it
        // hoop against rail.
        Serial.printf("        sub = star %lu hoops %lu world %lu hud %lu mir %lu sfx %lu sxr %lu vc %d fb %d\n",
                      (unsigned long)(acc_star / frames),
                      (unsigned long)(acc_aren / frames),
                      (unsigned long)(acc_wrld / frames),
                      (unsigned long)(acc_hud  / frames),
                      (unsigned long)(acc_mir  / frames),
                      (unsigned long)(acc_sfx  / frames),
                      (unsigned long)(acc_sxr  / frames),
                      vg_synth_live(),
                      vg_fire_live());
        // The halves, and the gap between them is the lever: `sub` is the slower one.
        // `denied` is a running total, not a per-window figure: it should be 0 for ever, and
        // any number at all is worth seeing rather than averaging away.
        // WHAT IS WATCHED AND WHAT IS ASKED FOR.
        //
        // Everything below this until the frame histogram used to print every two seconds.
        // Twelve lines a window is more than a terminal shows, so the ones that matter --
        // the phases, the band budget, the distribution -- were being pushed off the top by
        // splits that had already answered their question months or hours ago.
        //
        // The counters are all still collected; only the printing is on request. Serial 'd'
        // gives one window's full breakdown. See vg_capture_want_detail.
        const bool deep = vg_capture_want_detail();
        if (deep)
        // A WEDGE IS VISIBLE THE SECOND IT STARTS, which is what panel_reap's note has
        // always claimed and could not deliver while it was a printf inside the flush.
        if (g_panel_wedges)
            Serial.printf("        PANEL: %lu DMA transfer(s) abandoned -- the picture is "
                          "being degraded to keep the frame alive\n",
                          (unsigned long)g_panel_wedges);
        Serial.printf("        i2c  = touch %lu (lock %lu) of in %lu | denied %lu\n",
                      (unsigned long)(acc_touch / frames),
                      (unsigned long)(acc_lock  / frames),
                      (unsigned long)(acc_input / frames),
                      (unsigned long)vg_i2c_denied());
        if (deep)
        Serial.printf("        half = A %lu (core 1) B %lu (core 0)\n",
                      (unsigned long)(acc_suba / frames),
                      (unsigned long)(acc_subb / frames));
        // WHAT GROUP B IS MADE OF. `rest` is by subtraction -- the glitch and shake
        // arithmetic and whatever else is not one of the named calls -- so it costs no
        // second bracket and cannot double-count. See vg_prof.h.
        if (deep) {
            const uint32_t b   = acc_subb / frames;
            const uint32_t named = acc_slock / frames + acc_scan_p / frames
                                 + acc_smarks / frames + acc_sover / frames
                                 + acc_hud / frames + acc_mir / frames;
            Serial.printf("        subB = lock %lu canopy %lu hud %lu mir %lu marks %lu "
                          "over %lu rest %lu of %lu\n",
                          (unsigned long)(acc_slock / frames),
                          (unsigned long)(acc_scan_p / frames),
                          (unsigned long)(acc_hud / frames),
                          (unsigned long)(acc_mir / frames),
                          (unsigned long)(acc_smarks / frames),
                          (unsigned long)(acc_sover / frames),
                          (unsigned long)(b > named ? b - named : 0),
                          (unsigned long)b);
        }
        if (deep) {
            const uint32_t w = acc_wrld / frames;
            uint32_t named = 0;
            for (int i = 0; i < 6; i++) named += acc_w[i] / frames;
            Serial.printf("        world = motes %lu rocks %lu trails %lu ships %lu "
                          "msl %lu fire %lu rest %lu of %lu\n",
                          (unsigned long)(acc_w[0] / frames),
                          (unsigned long)(acc_w[1] / frames),
                          (unsigned long)(acc_w[2] / frames),
                          (unsigned long)(acc_w[3] / frames),
                          (unsigned long)(acc_w[4] / frames),
                          (unsigned long)(acc_w[5] / frames),
                          (unsigned long)(w > named ? w - named : 0),
                          (unsigned long)w);
        }
        if (deep)
        Serial.printf("        grid = hoops %lu (core 1) rails %lu (core 0)\n",
                      (unsigned long)(acc_ahoop / frames),
                      (unsigned long)(acc_arail / frames));
        // THE UPDATE, and it is all on the critical path -- serial, and before the flush.
        // `oth` is the rest of the state's own function plus the replay note, by
        // SUBTRACTION rather than by a tenth bracket. It can read 0: the spans are
        // integer microseconds and rounding across a window can just cover the total.
        if (deep) {
            uint32_t named = 0;
            for (int i = 0; i < 11; i++) named += acc_u[i] / frames;
            const uint32_t tot = acc_update / frames;
            Serial.printf("        upd  = pre %lu ship %lu arena %lu sky %lu field %lu "
                          "trail %lu enemy %lu ord %lu vfx %lu ai %lu cbt %lu "
                          "oth %lu of %lu\n",
                          (unsigned long)(acc_u[0] / frames),
                          (unsigned long)(acc_u[1] / frames),
                          (unsigned long)(acc_u[2] / frames),
                          (unsigned long)(acc_u[3] / frames),
                          (unsigned long)(acc_u[4] / frames),
                          (unsigned long)(acc_u[5] / frames),
                          (unsigned long)(acc_u[6] / frames),
                          (unsigned long)(acc_u[7] / frames),
                          (unsigned long)(acc_u[8] / frames),
                          (unsigned long)(acc_u[9] / frames),
                          (unsigned long)(acc_u[10] / frames),
                          (unsigned long)(tot > named ? tot - named : 0),
                          (unsigned long)tot);
        }
#if VG_DEBUG_TILT
        Serial.printf("   accel %.3f %.3f %.3f -> pitch %.2f yaw %.2f thr %.2f\n",
                      (double)in.raw_ax, (double)in.raw_ay, (double)in.raw_az,
                      (double)in.pitch, (double)in.yaw, (double)in.throttle);
#endif
        // A third line, and the one to read first when the question is "why is
        // this not 60". p50 is what the game normally does; the gap between p50
        // and p95 is how consistent it is; and the worst frame's split names the
        // phase to go and look at.
        //
        // Percentiles come out of the histogram by counting, and a bucket is
        // reported by its UPPER edge -- so "p95 18000" means 95% of frames came
        // in under 18.0 ms, which is the direction that cannot flatter the
        // result. The bottom bucket is everything at or under 12.0 ms.
        {
            uint32_t tot = 0;
            for (int i = 0; i < FT_BUCKETS; i++) tot += ft_hist[i];
            uint32_t p50 = 0, p95 = 0, seen = 0;
            const uint32_t n50 = tot / 2, n95 = (tot * 95 + 99) / 100;
            for (int i = 0; i < FT_BUCKETS; i++) {
                seen += ft_hist[i];
                const uint32_t edge = FT_BASE_US + (uint32_t)(i + 1) * FT_STEP_US;
                if (!p50 && seen >= n50) p50 = edge;
                if (!p95 && seen >= n95) p95 = edge;
            }
            const uint32_t late = ft_late;   // exact; see the note above
            if (tot) {
                Serial.printf("        frames %lu | p50 %lu p95 %lu late %lu (%lu%%) "
                              "| worst %lu us = in %lu upd %lu sub %lu blit %lu, state %u\n",
                              (unsigned long)tot,
                              (unsigned long)p50, (unsigned long)p95,
                              (unsigned long)late,
                              (unsigned long)(late * 100 / tot),
                              (unsigned long)ft_worst,
                              (unsigned long)ft_w_in, (unsigned long)ft_w_upd,
                              (unsigned long)ft_w_sub, (unsigned long)ft_w_blit,
                              (unsigned)ft_w_state);
            }
        }
        // Audio delivery, on its own line because it is not a frame-time number
        // and reading it as one would be a mistake. Samples, at 22050 a second:
        // 22 of them is a millisecond. See vg_prof.h for the decision this makes.
        // Audio delivery. The window is the same two seconds as the lines above,
        // so `blocked` reads straight off as a percentage: 1700 of 2000 ms waiting
        // for ring space means the queue is full 85% of the time, which is health
        // and not a problem. Near zero is the fault. See vg_prof.h.
        Serial.printf("        aud = blocked %lu ms/2s short %lu | peak %.2f clip %lu"
                      " | hud = radar %lu thr %lu rest %lu\n",
                      (unsigned long)(g_audio_blocked_us / 1000u),
                      (unsigned long)g_audio_short,
                      (double)g_synth_peak,
                      (unsigned long)g_synth_knee,
                      (unsigned long)(acc_hud_radar / frames),
                      (unsigned long)(acc_hud_thr   / frames),
                      // By subtraction, and floored: the two timed pieces are
                      // inside the same bracket that produces acc_hud, so a torn
                      // read cannot make this negative and wrap.
                      (unsigned long)(acc_hud > acc_hud_radar + acc_hud_thr
                                      ? (acc_hud - acc_hud_radar - acc_hud_thr) / frames
                                      : 0));
        // A fifth line, and the one that says whether band work is worth moving:
        // the raster cost of each band against the 768 us it has to fit inside.
        // An even overshoot is a rasteriser problem; two tall numbers in the
        // middle of the list are the ships, and those are reachable.
        {
            char row[NUM_BANDS * 6 + 1];
            int  at = 0;
            for (int i = 0; i < NUM_BANDS && at < (int)sizeof(row) - 1; i++)
                at += snprintf(row + at, sizeof(row) - at, "%lu ",
                               (unsigned long)(acc_band[i] / frames));
            Serial.printf("        band us/768 = %s\n", row);
        }
        {
            // Peak against cap, per slice. Sized down only from this.
            char row[96];
            int  at = 0;
            for (int i = 0; i < vg_rast_slices() && at < (int)sizeof(row) - 1; i++)
                at += snprintf(row + at, sizeof(row) - at, "%d/%d ",
                               vg_rast_slice_peak(i), vg_rast_slice_cap(i));
            Serial.printf("        slice peak/cap = %s\n", row);
        }
        ft_skip_next = true;
        vg_crumb(CRUMB_TAIL, (uint8_t)vg.state);
    }

    // The window closes on time whether or not the report went out -- which is
    // why this is a second block and not the tail of the one above.
    if (win) {
        report_ms = ms;
        g_audio_blocked_us = 0;
        g_audio_short      = 0;
        g_synth_peak       = 0.0f;
        g_synth_knee       = 0;
        for (int i = 0; i < FT_BUCKETS; i++) ft_hist[i] = 0;
        ft_worst = 0;
        ft_late  = 0;
        // ft_skip_next belongs to the PRINTING, not to the window: it exists to
        // keep the frame that paid for the formatting out of the histogram.

        for (int i = 0; i < 11; i++) acc_u[i] = 0;
    for (int i = 0; i < 6; i++) acc_w[i] = 0;
    acc_slock = acc_scan_p = acc_smarks = acc_sover = 0;
    acc_input = acc_update = acc_submit = acc_flush = 0;
        acc_rast  = acc_wait = acc_push = 0;
        acc_over_us = acc_over_n = 0;
    acc_canh = acc_canw = 0;
    acc_idle0 = 0;
        acc_join = acc_res = 0;
        acc_can = 0;
        acc_join_mm = acc_join_n = 0;
        for (int i = 0; i < NUM_BANDS; i++) acc_band[i] = 0;
        acc_sky   = acc_prim = acc_scan = 0;
        acc_aa = acc_ln = acc_tri2 = acc_oth = acc_tint = acc_mir = 0;
        acc_pt = acc_gl = acc_fl = 0;
        acc_lnpx = acc_lnn = 0;
        acc_star = acc_aren = acc_wrld = acc_hud = acc_sfx = acc_sxr = 0;
        acc_hud_radar = acc_hud_thr = 0;
        acc_ahoop = acc_arail = 0;
        acc_suba = acc_subb = 0;
        acc_touch = acc_lock = 0;
        frames = 0;
    }
}
