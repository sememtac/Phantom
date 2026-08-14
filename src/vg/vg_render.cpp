#include "vg_render.h"
#include "vg_states.h"
#include <Arduino.h>
#include "vg_sky.h"
#include "vg_draw.h"
#include "vg_prof.h"
#include "vg_game.h"
#include "vg_screens.h"
#include "vg_glitch.h"
#include "vg_course.h"
#include "vg_shake.h"
#include "vg_surge.h"
#include "vg_tv.h"
#include "vg_canopy.h"     // vg_canopy_current: is there a cockpit to redden
#include <stdio.h>
#include <math.h>

// THE SUBMIT COUNTERS, defined here because this file writes every one of them.
// Declared in vg_prof.h, which says what each measures and what it may not be read
// as -- g_sub_hud in particular brackets vg_draw_hud alone and is not group B's total.
uint32_t g_sub_star, g_sub_arena, g_sub_world, g_sub_hud;   // per-layer submit
uint32_t g_sub_a, g_sub_b;                                  // each submit half's wall time
uint32_t g_sub_lock, g_sub_canopy, g_sub_marks, g_sub_over; // group B, named

// Frame orchestration only. Every actual drawing routine lives in a vg_draw_*
// or vg_hud/vg_overlay module; what remains here is the part that is genuinely
// global -- the draw ORDER, and which layers get the spherical warp.

// Frame rate, bottom left, during flight only. The menus sit comfortably above
// 60 and have nothing to report; the number is there to be read while something
// is actually costing frame time. Held clear of the rounded corner by SCR_SAFE
// and drawn last so nothing can cover it.
static void draw_fps(float fps) {
    char buf[24];
    // Primitive count rides alongside it. When the rate dips, the single most
    // useful thing to know is whether the count went up with it -- geometry --
    // or stayed flat, which points at per-pixel fill instead. Without that the
    // two are indistinguishable from the outside.
    snprintf(buf, sizeof(buf), "%d FPS  %dP",
             (int)(fps + 0.5f), vg_rast_prim_count());
    vg_text(SCR_SAFE, SCR_H - SCR_SAFE - 14, buf,
            fps >= 59.0f ? INK_BRIGHT : INK_FAINT, 2);
}

// Hermite ease between two edges. The transition curves want acceleration at
// both ends -- a linear wipe reads as a slide, not as a tube.
static inline float smoothstep(float e0, float e1, float x) {
    if (e1 <= e0) return (x < e0) ? 0.0f : 1.0f;
    float t = (x - e0) / (e1 - e0);
    if (t < 0.0f) t = 0.0f;
    if (t > 1.0f) t = 1.0f;
    return t * t * (3.0f - 2.0f * t);
}

// The rear-view patch: the same world, half a turn about the vertical, drawn
// small in the top right.
//
// A SECOND SUBMISSION OF THE SCENE, which is the expensive stage -- submit bills
// frame time directly while the band raster hides under DMA. Three things keep
// it affordable. The starfield is left out, because at a quarter scale a field
// of single pixels is noise and it is the largest primitive count in the frame.
// Fills are off, so no triangle work. And the patch is clipped at submit, so
// everything outside it is discarded before it can reach the band lists.
// How much submit time the mirror costs, measured around the whole patch draw.
static uint32_t s_mirror_us = 0;
uint32_t vg_render_mirror_us(void) { return s_mirror_us; }

static void draw_rear_patch(const VgCam& base, float warp) {
    const uint32_t t0 = micros();

    // RIDES THE PANEL, RIGIDLY. The patch is still not warped -- bending a
    // viewport would bend the picture inside it -- but it no longer sits still
    // while the instruments around it flex, which made it read as pasted on
    // rather than set in.
    //
    // The whole window moves as one: backdrop, clip rect, projection offset and
    // frame all take the same offset, so the picture and its bezel stay
    // congruent and the reflection CANNOT land outside the frame. That matters
    // more than it sounds -- HUD_WARP_K is negative, so the bend pulls this
    // corner of the panel inward by tens of pixels at full throttle. Warping
    // only the frame would have left the picture hanging outboard of it.
    //
    // Whole pixels. The warp is quantised upstream for the same reason: a patch
    // sliding by fractions of a pixel reads as shimmer, not as motion.
    float wx, wy, rx, ry;
    vg_hud_warp_at(warp,          REAR_CX, REAR_CY, &wx, &wy);
    vg_hud_warp_at(REAR_WARP_REF, REAR_CX, REAR_CY, &rx, &ry);
    const int ox = (int)lroundf(wx - rx);
    const int oy = (int)lroundf(wy - ry);
    const int px = REAR_X + ox, py = REAR_Y + oy;
    // The backdrop, aft, which is also what hides the forward scene inside the
    // patch. It was a flat INK_WELL fill: opaque was the requirement, and a sky
    // is opaque too.
    //
    // It cannot ride in the band clear with the main sky, which is what the
    // first attempt did. The clear runs before anything is drawn, so the forward
    // world -- which has no reason to avoid the patch -- was painted straight
    // over it. As a primitive it lands here in submission order instead: after
    // the world, before the patch's own geometry.
    vg_sky_set_patch(px, py, REAR_W, REAR_H);
    vg_sky_patch_prim(px, py, REAR_W, REAR_H);

    VgCam rc = base;
    rc.rear  = true;
    rc.lite  = true;    // no contrails: see VgCam::lite
    rc.focal = FOCAL * REAR_FOCAL_K;
    // vg_project centres on the screen, and the shake offset is already a plain
    // screen-space translation added after the divide -- so it is exactly the
    // hook needed to move the whole projection into the corner. The patch does
    // not shake with the airframe, which is right: it is a repeater, not a
    // window in the canopy.
    rc.sx = REAR_CX + (float)ox - SCR_CX;
    rc.sy = REAR_CY + (float)oy - SCR_CY;

    vg_rast_viewport(px, py, REAR_W, REAR_H);
    vg_rast_fills(false);
    // The patch is world content, and it is drawn after the main view has already
    // switched AA back on for the instruments -- so it has to say so again.
    vg_line_aa_mode(false);
    vg_draw_arena_grid(rc, ARENA_GRID_ALL);
    vg_draw_world(rc);
    if (vg.state == VG_COURSE) vg_course_draw(rc);
    // The diamonds below are an instrument, and so is the bezel after them.
    vg_line_aa_mode(true);
    // Threat diamonds in the mirror, INSIDE the viewport bracket so they are
    // clipped to the patch like everything else in it. Bounds are the patch,
    // inset a little, and the half-size is clamped to 2..6px -- the main view's
    // 22 would be half the height of the window. A round closing from behind is
    // the single most useful thing this instrument shows, so it earns the four
    // strokes even here.
    vg_draw_missile_markers(rc, (float)(px + 3), (float)(py + 3),
                            (float)(px + REAR_W - 3), (float)(py + REAR_H - 3),
                            2.0f, 6.0f);
    vg_rast_fills(true);
    vg_rast_viewport_full();

    // Frame last, over the picture, in the system colour. One pixel outside the
    // clip rect on every side, so the picture is bounded by the bezel and the
    // bezel is never overdrawn by it.
    vg_rect(px - 1, py - 1, REAR_W + 2, REAR_H + 2, COL_HUD);
    s_mirror_us = micros() - t0;
}

// ===========================================================================
// SUBMIT ON BOTH CORES
//
// Submit is 3.8 ms of the 4.37 ms that bills frame time directly -- the panel eats
// 12.3 ms of every frame whatever the CPU does -- and combat was spending 5.9. It
// splits in half where the draw order already breaks, and the halves are nearly
// even: the world at 2.0 ms against the instruments at 1.8.
//
// So core 1 builds the world while core 0 builds the instruments, into two slices
// of the one primitive array that vg_prim_join then makes contiguous. Neither half
// writes game state and every piece of submit state that used to be global is now
// per-submitter, so there is nothing left for them to fight over.
//
// A GO/DONE PAIR rather than a queue. There is exactly one item of work per frame
// and the render thread cannot proceed past it, so a queue would be machinery
// around a rendezvous. Two binary semaphores are a couple of microseconds against
// the 1.8 ms they overlap.
//
// STARTED LAZILY, like the sensor task, so nothing depends on a host calling an
// init in a particular order -- the game is meant to drop into another firmware
// whose launcher does its own setup. Until the task exists, and on any board where
// it cannot be created, kick_instruments returns false and the caller submits group
// B itself on this thread. That path is the old serial code exactly.
// ===========================================================================
static void submit_instruments(const VgCam& cam, const VgInput* in, float fps);

static SemaphoreHandle_t s_gb_go   = nullptr;
static SemaphoreHandle_t s_gb_done = nullptr;

// The frame's arguments, handed over by the kick. Written by the render thread
// before the give and read by core 0 after the take, so the semaphore is the
// barrier that publishes them -- no other synchronisation is needed or wanted.
static VgCam          s_gb_cam;
static const VgInput* s_gb_in  = nullptr;
static float          s_gb_fps = 0.0f;

static void instruments_task(void*) {
    for (;;) {
        xSemaphoreTake(s_gb_go, portMAX_DELAY);
        submit_instruments(s_gb_cam, s_gb_in, s_gb_fps);
        xSemaphoreGive(s_gb_done);
    }
}

static bool kick_instruments(const VgCam& cam, const VgInput* in, float fps) {
    if (!s_gb_go) {
        s_gb_go   = xSemaphoreCreateBinary();
        s_gb_done = xSemaphoreCreateBinary();
        if (!s_gb_go || !s_gb_done) return false;
        // Priority 3: above the audio task and the sensors, because the render
        // thread is BLOCKED on this one -- a frame cannot finish without it, where
        // a late audio chunk only eats into a 65 ms ring. It yields by blocking on
        // the semaphore, so it cannot starve either of them.
        if (xTaskCreatePinnedToCore(instruments_task, "subB", 4096, nullptr, 3,
                                    nullptr, 0) != pdPASS) {
            s_gb_go = nullptr;
            return false;
        }
    }
    s_gb_cam = cam;
    s_gb_in  = in;
    s_gb_fps = fps;
    xSemaphoreGive(s_gb_go);
    return true;
}

static void await_instruments(void) {
    // Bounded, and generous: this is a 1.8 ms job. If it ever times out the frame
    // goes out with half its instruments rather than the game stopping, and the
    // primitive count in the corner will show it.
    if (xSemaphoreTake(s_gb_done, pdMS_TO_TICKS(100)) != pdTRUE)
        Serial.println("vg_render: group B did not finish");
}

void vg_render_frame(const VgInput* in, float fps) {
    VgCam cam = vg_cam_make(vg.bank, vg_shake.x, vg_shake.y, vg.cam_zoom);

    // Looking aft fills the main window. The patch is the button as well as the
    // repeater, so the picture the player is already watching is the one that
    // grows -- there is nothing to reacquire.
    const bool rear_view = vg.rear_view;
    cam.rear = rear_view;

    // The backdrop is filled during the band flush, long after this, and has no
    // camera to consult -- so it is told. This is the MAIN WINDOW's direction
    // only. The patch shows sky too, but it is permanently aft whatever the main
    // window is doing, so vg_sky_fill_patch forces the rear branch itself rather
    // than reading this.
    vg_sky_set_rear(rear_view);

    // And the cockpit is not drawn while looking aft. The canopy is the front of the ship;
    // over a rear view it would claim you are facing the other way. The intro's world gate
    // still runs -- see vg_canopy_rear.
    vg_canopy_rear(rear_view);

    // How red the whole picture goes. Decided here because this is the layer that
    // knows both the wall distance and the rasteriser; the rasteriser itself has
    // no idea a wall exists. Only while flying: a menu has no boundary to hit,
    // and the attract loop tinting itself red would be nonsense.
    float tint = 0.0f;
    // VGS_LIVE, which includes the course deliberately: meeting the wall warning
    // somewhere free is the reason one gate in four is placed against the
    // boundary.
    if ((vg_state_flags(vg.state) & VGS_LIVE) &&
        vg.wall_clear < ARENA_TINT_RANGE) {
        tint = 1.0f - vg.wall_clear / ARENA_TINT_RANGE;
    }
    // THE WARNING IS ON THE COCKPIT, and the ring is the FALLBACK. Measured at the
    // boundary: sky 3060 -> 1824 us and 53.1 -> 59.7 fps, because the ring tints the
    // backdrop FILL per pixel over a large annulus, while this is a different entry in a
    // colour table the cockpit already reads. It costs nothing per pixel.
    //
    // WHY THERE WAS A RING AT ALL, which is worth keeping now that it is being replaced:
    // it was written before canopies existed, when there was no cockpit to light up and
    // tinting the whole view was the only way to say "wall" at all. It was heavy-handed
    // because it had to be. The cost of the canopy texture is already paid on every frame
    // that draws one, so a hull that HAS a cockpit should spend that instead of buying a
    // second full-screen effect.
    //
    // A hull with no drawing still needs telling. Three of the four have none, and they
    // flew to the wall in silence for one commit because this was switched off
    // unconditionally. So the ring remains, for exactly the case it was invented for.
    //
    // AND IT FLASHES once the wall is close enough that turning is urgent -- see
    // CANOPY_ALARM_FLASH_AT. A steady ramp cannot say "now", only "closer".
    // SECONDS TO IMPACT AT THE CURRENT CLOSING RATE, and only while the clearance is
    // actually being spent -- see vg.wall_rate. RATE_MIN keeps a ship holding station from
    // strobing on numerical noise.
    //
    // The colour stays what the clearance asked for. What the countdown drives is the
    // STROBE: white for CANOPY_ALARM_ON_SECS, then back to the red, faster as the seconds
    // run out. Two signals from two facts -- see the note in cfg_hud.h.
    bool white = false;
    if (tint > 0.0f && vg.wall_rate > CANOPY_ALARM_RATE_MIN) {
        const float ttc = vg.wall_clear / vg.wall_rate;
        if (ttc < CANOPY_ALARM_SECS) {
            // 0 at the threshold, 1 where the rate tops out.
            float u = (CANOPY_ALARM_SECS - ttc)
                    / (CANOPY_ALARM_SECS - CANOPY_ALARM_SECS_MAX);
            if (u < 0.0f) u = 0.0f; else if (u > 1.0f) u = 1.0f;
            const float hz = CANOPY_ALARM_HZ_MIN
                           + (CANOPY_ALARM_HZ_MAX - CANOPY_ALARM_HZ_MIN) * u;
            // vg.state_t is the clock the rest of the cockpit runs on, so the strobe cannot
            // drift against it. fmodf gives the position within one flash period.
            const float period = 1.0f / hz;
            white = fmodf(vg.state_t, period) < CANOPY_ALARM_ON_SECS;
        }
    }
    // THE COCKPIT IS THE ONLY WARNING, by the author's decision, and a hull with no
    // drawing gets none until one is drawn for it. The ring is gone rather than kept as a
    // fallback: it was invented because there was no cockpit to light, and keeping a
    // second full-screen effect alive for three hulls that are getting canopies anyway is
    // paying twice for a problem that is being solved once.
    vg_canopy_alarm(tint, white);

    // The set turning on and off. Both directions are the same two phases, but
    // they are not mirror images and each control has its own timing, which is
    // why this is three curves and not one progress number.
    //
    // OUT: the picture fades, the aperture closes on it, and the scan band comes
    // up as it goes -- so the last thing on screen is the band and not a shrunken
    // copy of the scene. IN: the band is already lit, holds a moment, then opens
    // while the picture comes back up underneath it.
    if (vg_tv.phase == TV_NONE) {
        vg_tv_set(1.0f, 1.0f, 0.0f, 0.0f);
    } else if (vg_tv.phase == TV_HOLD) {
        vg_tv_set(0.0f, 0.0f, 0.0f, 1.0f);      // dead air
    } else {
        // ONE CURVE, RUN BOTH WAYS. `c` is how far the set is toward being off:
        // 0 is a picture, 1 is a dot about to vanish. Going out it runs forward,
        // coming back it runs in reverse, so the two are the same shape by
        // construction rather than by two sets of numbers agreeing.
        //
        // They were separate before, and the turn-on had drifted into something
        // else entirely -- the white resolving away before the bar had finished
        // opening, which reads as two bands travelling apart rather than as one
        // line growing out of the middle.
        //
        // Phases, in the order they happen going OUT:
        //   the picture darkens                  dim  rises first
        //   it collapses to the centre line      open falls, 0.05..0.62
        //   what is left whitens                 wash rises
        //   the line shortens to a dot and goes  wide falls, 0.62..1.00
        //
        // THE TWO SHRINKS DO NOT OVERLAP. They used to: the line began losing its
        // width while it was still losing its height, so the dot phase was over
        // in a couple of frames and read as a flash rather than as a motion.
        // Handing the horizontal its own stretch of the curve -- more than a
        // third of it -- is what makes a dot something the eye can follow out of
        // the middle of the screen, and it costs the collapse nothing.
        const float c = (vg_tv.phase == TV_OUT) ? (vg_tv.t / TV_OUT_TIME)
                                                : (1.0f - vg_tv.t / TV_IN_TIME);
        vg_tv_set(1.0f - smoothstep(0.05f, 0.62f, c),
                   1.0f - smoothstep(0.62f, 1.00f, c),
                   // Lit for essentially the whole of it. The kill at the very end
                   // is what makes the dot go OUT rather than merely get small.
                   smoothstep(0.20f, 0.55f, c) * (1.0f - smoothstep(0.985f, 1.0f, c)),
                   smoothstep(0.00f, 0.50f, c));
    }

    vg_rast_begin_frame();

    // World, back to front. The boundary goes down before anything solid so the
    // hidden-line fills occlude it.
    // Submit, timed per layer. "sub 11ms" names a stage; this names the layer.
    // Zeroed every frame. A frame that skips a layer must report zero for it,
    // not last flight's number -- the stale HUD value made menu frames claim
    // more submit time than the whole stage took.
    g_sub_star = g_sub_arena = g_sub_world = g_sub_hud = 0;
    // NO ANTIALIASING IN THE WORLD. AUTHOR'S CALL, and the reasoning is the
    // player's: the HUD carries nearly everything the player needs to know, and a
    // dogfight gives the ship itself very little screen -- contacts are small,
    // distant and moving unpredictably, so a smoothed hull edge buys almost
    // nothing while an instrument that has to be READ buys a great deal.
    //
    // It is also where the money is. An AA span costs per PIXEL, so its price
    // tracks LENGTH -- arena_seg's note works this out in full -- and in combat
    // the long spans are near ship hulls. Measured at 1158 us of a 21 ms frame,
    // alongside 154 triangles of solids the attract loop never draws, which is
    // why the first measurement of this missed it completely.
    //
    // The grid, the motes, the debris, the trails and the course gate had each
    // reached this conclusion locally and switched AA off for themselves. This is
    // the same decision made once, for the whole layer, so the next thing added
    // to the world does not have to rediscover it.
    // GROUP A: the world. See the note on Sub in vg_raster.cpp -- submit splits in
    // half here because the draw order already does, and the two halves can be
    // built at the same time on the two cores.
    // HANDED TO CORE 0 HERE -- before the world rather than after, so the two
    // halves overlap for as long as possible. The wait is at the end of this
    // function, and group B is the shorter half (1.8 ms against 2.0), so in
    // practice it has finished by the time the world is done and the wait is free.
    const uint32_t t_half_a = micros();
    const bool async = kick_instruments(cam, in, fps);

    vg_prim_select(0);
    vg_line_aa_mode(false);

    uint32_t t_sub = micros();
    vg_draw_starfield(cam);
    g_sub_star = micros() - t_sub;  t_sub = micros();
    // THE WHOLE GRID, back on this core, and the reason is a measurement rather than a
    // preference.
    //
    // The rails were moved to core 0 to balance submit, on figures that turned out to be the
    // wrong ones: g_sub_hud was read as group B's total when it only brackets vg_draw_hud, and
    // group B also carries the rear-view patch, the markers and the overlays. Timing both
    // halves end to end says core 0 is the HEAVIER one in combat --
    //
    //   A 1942 (starfield, hoops, world)      B 2895 (rails, instruments)
    //
    // -- so moving 570 us of rails onto it made the gate worse, not better. Back here the two
    // come out 2512 against 2325, which is within 100 us of the best a two-way split can do.
    //
    // THIS PICKS A REGIME, and combat is the right one to pick: the attract loop is the other
    // way round, A gating with B nearly empty, and it runs at 66-71 fps where nothing needs
    // help. If a future scene flips it again, the fix is not another static guess -- it is to
    // choose the rails' owner per frame from g_sub_a and g_sub_b, which the slices already
    // allow because a slice's position in the join does not depend on which core filled it.
    vg_draw_arena_grid(cam, ARENA_GRID_ALL);
    g_sub_arena = micros() - t_sub; t_sub = micros();
    // AND THE OBJECTS INTO SLICE 2, which is what leaves room for core 0's rails to
    // land between the grid and the hulls rather than on top of them.
    vg_prim_select(2);
    // AND AA OFF AGAIN, because the setting lives in the SLICE. vg_line_aa_mode(false)
    // above applied to slice 0; slice 2 starts from begin_frame's default, which is ON.
    // Without this the hulls would be antialiased -- against the author's explicit call
    // above, and at a cost that scales with span length exactly where the spans are
    // longest.
    vg_line_aa_mode(false);
    vg_draw_world(cam);
    g_sub_world = micros() - t_sub;
    // Gated on the state, not just on ring_alive. A stale gate drawn into a match
    // would be confusing at best, and its normal is only guaranteed sane while
    // the course owns it.
    if (vg.state == VG_COURSE) vg_course_draw(cam);

    // Collect core 0. If it was never started -- the first frame, or a board where
    // the task could not be created -- group B has not been submitted at all, so it
    // happens here instead, on this thread, exactly as it used to.
    // BEFORE the await, so this is core 1's own work and not the rendezvous -- what the
    // wait costs is exactly the gap between the halves, which is the thing being measured.
    g_sub_a = micros() - t_half_a;

    if (async) await_instruments();
    else       submit_instruments(cam, in, fps);

    // THE FRAME COUNTER LAST, AND ON THIS THREAD, because it prints the primitive
    // count INTO THE FRAME and that count is only whole once both halves are in.
    // Left in group B it ran on core 0 while the world was still being submitted, so
    // the number it read was a moving target and the frame stopped being
    // reproducible -- which the replay harness would have called a rendering bug.
    //
    // Into group B's SLICE, so it still lands last in draw order exactly where it
    // was. And gated on the same condition as group B's early return, so a menu
    // frame still does not get one.
    if (!vg_state_is_menu(vg.state)) {
        vg_prim_select(3);
        draw_fps(fps);
    }
}

// ===========================================================================
// GROUP B: the instruments, and core 0 runs it.
//
// Everything from the antialiasing boundary to the end of the frame -- the lock
// box, the HUD, the rear-view patch, the markers, the overlays, the captions.
// Lifted into a function for exactly one reason, so the other core can call it;
// the body is unchanged from when it was inline.
//
// SAFE TO RUN BESIDE GROUP A because it reads `vg` and writes nothing to it -- true
// of all six draw modules, since vg_cockpit's writes live in vg_update_alerts and
// vg_hud_decay, which are update functions. The rasteriser side took more work: see
// the note on Sub in vg_raster.cpp for the eight pieces of submit state that had to
// stop being global before this was safe there too.
//
// The early return for menu states is the original one, and it is why the join has
// to live in vg_rast_flush rather than at the end of submit.
// ===========================================================================
static void submit_instruments(const VgCam& cam, const VgInput* in, float fps) {
    const uint32_t t_half = micros();
    // Two locals that group A used to share with this code when the two were one
    // function. `rear_view` comes off the camera rather than being re-read from vg,
    // because the preamble set cam.rear from it -- same value, and it keeps this
    // half's inputs to what was handed in.
    uint32_t   t_sub     = micros();
    const bool rear_view = cam.rear;


    // ...AND ALL OF IT ON THE INSTRUMENTS, from here to the end of the frame.
    // Everything past this line is read rather than flown through: the reticle,
    // the bars, the radar, the captions. Static geometry a few pixels wide is
    // exactly where a half-pixel crawl shows and where smoothing earns its cost.
    //
    // GROUP B FROM HERE, and the AA boundary and the group boundary being the same
    // line is not a coincidence -- the reason the halves are separable is the same
    // reason they want different settings.
    // Closed on EVERY exit, including the menu return below, so the figure is the whole
    // half and not only the flying case. See g_sub_b in vg_prof.h.
    struct HalfB { uint32_t t0; ~HalfB() { g_sub_b = micros() - t0; } } half_b{t_half};
    (void)half_b;

    // THE RAILS ARE NOT DRAWN HERE ANY MORE -- see the note beside the grid call in group A.
    // This half is the heavier of the two in combat, so giving it more was the wrong direction.
    //
    // Slice 1 is left empty rather than removed. The join skips an empty block, so it costs a
    // compare a frame, and keeping it is what makes the rails' owner a one-line decision again
    // if the balance ever wants revisiting.
    vg_prim_select(3);
    vg_line_aa_mode(true);

    // Menus fly the idle scene underneath but carry no instruments -- a HUD on
    // the ship-select screen would be reporting on a fight that is not happening.
    if (vg_state_is_menu(vg.state)) {
        switch (vg.state) {
        case VG_ENTRY:   vg_draw_entry();    break;
        case VG_SELECT:  vg_draw_select();   break;
        case VG_BRACKET: vg_draw_bracket();  break;
        case VG_REPAIR:  vg_draw_repair();   break;
        default:         vg_draw_overlays(); break;
        }
        vg_draw_ift();
        return;
    }

    { const uint32_t t = micros(); vg_draw_lock_box(cam); g_sub_lock = micros() - t; }


    // Instruments are drawn onto the virtual canopy. The bend tracks speed, so
    // the canopy visibly flexes as you open the throttle -- but QUANTISED,
    // because a continuously varying warp shifts every HUD line by a fraction of
    // a pixel per frame, which reads as shimmer rather than motion.
    // Driven by the throttle COMMAND, not by actual speed: the canopy responds
    // to what you asked for rather than waiting out the flight model's
    // deliberately slow acceleration.
    float sn = vg.throttle_vis;
    if (sn < 0.0f) sn = 0.0f;
    if (sn > 1.0f) sn = 1.0f;

    float warp = HUD_WARP_SPEED_MIN + (1.0f - HUD_WARP_SPEED_MIN) * sn;
    // THE SURGE BOWS THE PANEL FURTHER, added before the quantiser rather than after it so
    // the bend still lands on one of HUD_WARP_STEPS levels -- an unquantised warp
    // re-subdivides every instrument primitive on a value that moves every frame, which is
    // what the quantiser was put here to stop. See vg_surge.h.
    warp += SURGE_HUD_WARP * vg_surge.level;
    warp = floorf(warp * HUD_WARP_STEPS + 0.5f) / HUD_WARP_STEPS;

    // THE CANOPY RELAXES AS THE SHIP ACCELERATES -- inverted against the instruments, and
    // deliberately. At rest the frame is at full bulge and sits close; opening the throttle
    // flattens it out, which reads as being pressed back into the seat rather than pulled
    // toward the glass.
    //
    // It also puts the cost in the right place. The warp is at its most expensive at amount 1,
    // and that is now IDLE -- where the scene is quiet -- while full throttle, when the trails
    // are longest and the bands are closest to overrunning, pays nothing at all. The two worst
    // cases no longer land on the same frame.
    //
    // Raw normalised throttle rather than `warp`, which starts at HUD_WARP_SPEED_MIN and is
    // never zero. This one has to reach both ends.
    //
    // HELD FLAT WHILE THE COCKPIT IS COMING ONLINE. The intro blacks the world out by region
    // and the regions are screen columns, so the frame has to be where the gate thinks it is --
    // and the resting warp is full bulge, which is the furthest it ever gets from flat. The flex
    // is zero through the sequence and ramps in after it, so the cockpit takes up its bulge
    // instead of appearing in it.
    //
    // AND FROZEN WHILE PAUSED, which is why `live` is read up here rather than where the shake
    // reads it thirty lines down.
    //
    // The canopy's flex and lag are the only cockpit state driven from the RENDERER. Everything
    // else that moves is stepped from vg_world_step, which vg_upd_pause deliberately does not
    // call -- so everything else stopped for free and these two did not. vg_canopy_lag advances
    // its spring once per CALL, with no dt, so a paused player leaning on the stick was still
    // feeding it: the frame kept swinging over a stopped world.
    //
    // Skipped rather than zeroed. The spring's state is left exactly as it was, so the frame HOLDS
    // its displacement instead of relaxing to centre while the picture is frozen -- a cockpit that
    // slid back to true during a pause would be the one thing in the shot still moving.
    const bool live = (vg.state != VG_PAUSE);

    const float flex = vg_canopy_intro_flex();
    // The frame flexes with the surge as well as with the throttle. ADDED, not substituted:
    // a cockpit that stopped responding to speed the moment the instruments failed would
    // read as the effect replacing the flight model rather than sitting on top of it.
    if (live) vg_canopy_warp((1.0f - sn) * flex + SURGE_FRAME_FLEX * vg_surge.level);
    // ...and the frame trails the ship, on all three axes. All three are the COMMAND, which is
    // what the frame should be late to -- the ship is already late to it itself, and lagging a
    // lag reads as sludge.
    //
    // ROLL COMES FROM in->roll, and reading vg.bank instead was simply wrong. Bank is the
    // COSMETIC lean a turn produces, driven from -yaw_in, and vg_flight drives it to ZERO while
    // the roll key is held -- so the one moment the shear was meant to fire was the one moment
    // its input was being cancelled. It was also an angle in radians against two sticks in
    // -1..1, so its per-frame change was an order of magnitude smaller even when it did move.
    // The shear was built and connected to nothing.
    //
    // Driven here rather than inside the warp, because it has to keep working when the warp
    // amount is zero.
    // The hull's character, compressed toward 1, times how open the throttle is. sn is the
    // same normalised throttle the warp uses -- and the warp runs the other way, so the frame
    // trades being close and still for being flat and alive as the ship gets moving.
    if (live) {
        const float shake = vg.spec ? vg.spec->shake : 1.0f;
        const float hull  = 1.0f + (shake - 1.0f) * CANOPY_LAG_HULL;
        // Agility, not throttle: the ship turns hardest with the throttle shut, so that is
        // where the frame should move most. vg.agility already carries the hull's own bonus and
        // malus, so the spread differs per ship without a second curve.
        const float agi = 1.0f + (vg.agility - 1.0f) * CANOPY_LAG_AGI;
        // Same gate as the warp, and for the same reason -- a frame that is trailing the ship is
        // not where the world gate expects it either.
        vg_canopy_lag(in ? in->yaw : 0.0f, in ? in->pitch : 0.0f,
                      in ? in->roll : 0.0f, hull * agi * flex);
    }

    // Instruments come up as a hologram catching: mostly absent at first,
    // flickering in, solid by the end. Driven by dropping whole frames rather
    // than by dimming, because a projector that is not holding sync loses the
    // image outright and a fade just looks like a brightness slider.
    //
    // AND NOT AT ALL until the cockpit calls for them. hud_boot is no longer set at the top of a
    // match -- the boot is a chain, and the instruments are the third link -- so a bare
    // `hud_boot > 0` test would have read as "already booted" through the whole dark phase and
    // drawn the panel solid from the first frame. Which is the opposite of the sequence.
    //
    // ...and the flicker holds while PAUSED. hud_boot does not decay during a pause -- it is
    // stepped from vg_world_step, which vg_upd_pause does not call -- but the flicker's own bucket
    // comes off vg.state_t, which RESETS on every state change and then runs. So pausing mid-boot
    // left the instruments strobing through a fresh pattern over a stopped world, forever. Held
    // solid instead: a paused picture should be a still.
    bool draw_instruments = vg.hud_cued;
    if (vg.hud_cued && vg.hud_boot > 0.0f && live) {
        const float p = 1.0f - vg.hud_boot / HUD_BOOT_TIME;   // 0..1 settled
        // Bucketed so the flicker has a rate of its own instead of strobing at
        // whatever the frame rate happens to be.
        const uint32_t bucket = (uint32_t)(vg.state_t * 40.0f);
        uint32_t h = bucket * 2654435761u;
        h ^= h >> 15;
        draw_instruments = ((h % 100u) < (uint32_t)(p * p * 118.0f));
    }

    // Avionics feel the airframe. The panel jitters at the top of the throttle
    // too, but smaller than the world does and on its own clock -- what sells it
    // is the two disagreeing. Shaking in lockstep would read as one bigger
    // shake; out of step, it reads as a rack that is not quite bolted down.
    //
    // `live` is declared above now, because the canopy needs it first.

    // Strain, from what the airframe is actually doing. The same number the camera
    // shake uses, so the panel and the world agree about how hard the ship is
    // working even though they jitter on separate clocks.
    const float strain = live ? vg.buzz : 0.0f;

    // Damage, and it escalates. A hit on a fresh hull is a flicker; the same hit
    // on a wreck nearly takes the display out, because the systems taking it
    // have already been carved up. `wound` scales the impulse, and past
    // DAMAGE_CHRONIC it also holds a permanent floor -- a badly hurt ship never
    // stops glitching, it only glitches less between hits.
    //
    // This is the hull bar said a second way. The number is precise and easy to
    // ignore mid-turn; a panel that will not hold still is neither.
    const float wound = (vg.health_max > 0.0f)
                      ? (1.0f - vg.health / vg.health_max) : 0.0f;

    float hurt = 0.0f;
    if (live) {
        if (vg.damage_glitch > 0.0f)
            hurt = (vg.damage_glitch / DAMAGE_GLITCH) * (0.30f + 0.70f * wound);
        if (wound > DAMAGE_CHRONIC) {
            const float chronic = (wound - DAMAGE_CHRONIC)
                                / (1.0f - DAMAGE_CHRONIC) * 0.34f;
            if (chronic > hurt) hurt = chronic;
        }
    }

    float jx = 0.0f, jy = 0.0f;
    if (strain > 0.0f)
        // Not squared again: vg.buzz already carries the speed-squared curve.
        vg_glitch_offset(vg.state_t, 47.0f, HUD_SHAKE_MAX * strain, &jx, &jy);
    if (hurt > 0.0f) {
        // A hit throws the panel much harder than speed ever does, and on its
        // own clock again so the two never resolve into one motion.
        float hx, hy;
        vg_glitch_offset(vg.state_t, 31.0f, HUD_SHAKE_MAX * 3.4f * hurt, &hx, &hy);
        jx += hx;
        jy += hy;
    }
    // The knock bus: warheads going off nearby, fighters crossing close, fire the
    // ship is inside. Its own clock again -- see vg_shake_hud. This is the
    // instruments feeling what the airframe felt, and it is deliberately the last
    // term, so the panel is never still while the view is being thrown about.
    if (live) {
        float kx, ky;
        vg_shake_hud(&kx, &ky);
        jx += kx;
        jy += ky;
        // AND THE SURGE SHAKES THE PANEL ITSELF, on its own clock. Its own frequency too --
        // SURGE_JITTER_HZ is kept well away from the damage glitch's 47.0, because two
        // offsets at neighbouring frequencies beat against each other into a slow throb that
        // reads as neither of them.
        if (vg_surge.level > 0.0f) {
            float sx, sy;
            vg_glitch_offset(vg.state_t, SURGE_JITTER_HZ,
                             SURGE_HUD_JITTER * vg_surge.level, &sx, &sy);
            jx += sx;
            jy += sy;
        }
    }

    // THE FRAME, BEFORE THE INSTRUMENTS AND WHETHER OR NOT THEY DRAW.
    //
    // Here rather than at the top of vg_draw_hud, which is where it was, because vg_draw_hud is
    // skipped on most frames while the instruments are catching -- and the cockpit is structure,
    // not projection. It has no business flickering with them.
    //
    // The intro makes it load-bearing: the world is held black by the canopy primitive, so a
    // frame without it is a frame of the whole world at full brightness in the middle of the
    // sequence. Outside the warp and jitter brackets because the table is panel-space pixels
    // and carries no coordinates -- neither ever applied to it.
    //
    // Submitted first, so every instrument still draws on top of it.
    { const uint32_t t = micros(); vg_canopy_prim(); g_sub_canopy = micros() - t; }

    if (draw_instruments) {
        vg_hud_jitter(jx, jy);
        vg_hud_warp(true, warp);
        t_sub = micros();
        vg_draw_hud(cam, in, fps);
        g_sub_hud = micros() - t_sub;
        vg_hud_warp(false, 1.0f);
        vg_hud_jitter(0.0f, 0.0f);

        // OUTSIDE the warp bracket. The patch is a flat repeater set into the
        // panel, and bending it would bend the picture inside it too -- the one
        // thing on the panel that is a window rather than an instrument.
        //
        // Not while the main view is already aft: the patch would then be a
        // small copy of the screen behind it, showing forward would contradict
        // the frame it is drawn in, and either way it is the button the player
        // is holding down.
        s_mirror_us = 0;
        // Drawn while paused too -- it went missing there as a performance cut
        // and the author sent it back: a dark hole in the panel reads as a
        // fault, and a pause menu is exactly when a player looks the cockpit
        // over. The motes stay out of pause; stillness is only wrong on things
        // whose meaning is motion.
        if (!rear_view) draw_rear_patch(cam, warp);
    }

    // Panel damage, at whichever severity is worse. Kept low for strain -- a
    // readout struggling, not failing, since anything more would make the
    // instruments unusable at exactly the moment they matter most.
    //
    // The strain term is capped before it is scaled. `strain` is vg.buzz now,
    // which deliberately runs PAST 1.0 for a light airframe at full throttle, and
    // this coefficient was chosen back when it could not. Without the cap a
    // CHARIOT would sit at a glitch level its hull never earned, just for going
    // fast -- which would say "damaged" using the vocabulary that means damaged.
    const float sev_strain = (strain > 1.0f ? 1.0f : strain) * 0.12f;
    const float sev = (hurt > sev_strain) ? hurt : sev_strain;
    if (sev > 0.0f) {
        vg_glitch_patches(vg.state_t, sev);
        // Tears only once it is genuinely bad. They displace the whole width of
        // the screen, so they are the loudest thing in the vocabulary and have
        // to stay the last thing it reaches for.
        if (hurt > 0.45f) vg_glitch_tears(vg.state_t, (hurt - 0.45f) * 1.4f);
    }

    // A scan bar running down the screen while it settles. One rectangle, and
    // it is what makes the flicker read as a projector finding its picture
    // rather than as a rendering fault.
    // Not while paused, for the reason the flicker above is not: hud_boot is frozen but state_t
    // is not, so the bar would go on sweeping a panel that has stopped booting.
    if (vg.hud_boot > 0.0f && live) {
        const float p  = 1.0f - vg.hud_boot / HUD_BOOT_TIME;
        const int   sy = (int)(fmodf(vg.state_t * 620.0f, (float)SCR_H));
        vg_fill_rect(0, sy, SCR_W, 2, vg_dim(INK_BRIGHT, 1.0f - p));
    }

    // Everything past here stays FLAT: the steering ring has to sit exactly under
    // the finger, and the markers have to line up with what they point at. A
    // curved control that does not match where you are touching feels broken
    // rather than physical.
    const uint32_t t_marks = micros();
    vg_draw_steer_indicator(in);
    vg_draw_target_markers(cam);
    vg_draw_missile_markers(cam, 26.0f, 26.0f, SCR_W - 26.0f, SCR_H - 26.0f,
                            7.0f, 22.0f);
    vg_draw_threat_indicator(cam);
    g_sub_marks = micros() - t_marks;

    { const uint32_t t = micros(); vg_draw_overlays(); g_sub_over = micros() - t; }

    // The broadcast sits ON TOP of the instruments, not under them.
    //
    // It was drawn before the HUD, on the reasoning that a line over the cutscene
    // must not compete with instruments that are not there yet. That is true and
    // it is not the constraint that matters: during a match the HUD then painted
    // straight over the band. Drawn here it works in both places, because the
    // cutscene has no instruments for it to be on top OF.
    //
    // After vg_draw_overlays too, so the intro's own fighter card cannot cover it.
    // Flat, like everything else past the warp: this is a caption laid over the
    // picture, not something bolted to the canopy.
    // NOT WHILE PAUSED. The pause screen covers most of it and the rest shows
    // through the gaps between the buttons, so a caption the player cannot act on
    // ends up threaded between the ones they can. The broadcast is part of the
    // game that is currently suspended.
    if (vg.state != VG_PAUSE) vg_draw_ift();

    // Last of all, over everything including the instruments.
    if (vg.state == VG_PAUSE) vg_draw_pause();
}
