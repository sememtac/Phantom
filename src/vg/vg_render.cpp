#include "vg_render.h"
#include "vg_draw.h"
#include "vg_game.h"
#include "vg_screens.h"
#include "vg_glitch.h"
#include "vg_course.h"
#include "vg_sim.h"
#include <stdio.h>
#include <math.h>

// Frame orchestration only. Every actual drawing routine lives in a vg_draw_*
// or vg_hud/vg_overlay module; what remains here is the part that is genuinely
// global -- the draw ORDER, and which layers get the spherical warp.

// Declared here rather than in vg_draw.h because these three are HUD pieces that
// must NOT be warped, so they are called from outside the warp bracket.
void vg_draw_lock_box(const VgCam& cam);
void vg_draw_steer_indicator(const VgInput* in);
void vg_draw_target_markers(const VgCam& cam);
void vg_draw_threat_indicator(const VgCam& cam);

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

void vg_render_frame(const VgInput* in, float fps) {
    VgCam cam = vg_cam_make(vg.bank, vg.shake_x, vg.shake_y, vg.cam_zoom);

    // How red the whole picture goes. Decided here because this is the layer that
    // knows both the wall distance and the rasteriser; the rasteriser itself has
    // no idea a wall exists. Only while flying: a menu has no boundary to hit,
    // and the attract loop tinting itself red would be nonsense.
    float tint = 0.0f;
    // VG_COURSE included deliberately: meeting the wall warning somewhere free
    // is the reason one gate in four is placed against the boundary.
    if ((vg.state == VG_PLAYING || vg.state == VG_HIT || vg.state == VG_COURSE) &&
        vg.wall_clear < ARENA_TINT_RANGE) {
        tint = 1.0f - vg.wall_clear / ARENA_TINT_RANGE;
    }
    vg_rast_tint(tint);

    // The set turning on and off. Both directions are the same two phases, but
    // they are not mirror images and each control has its own timing, which is
    // why this is three curves and not one progress number.
    //
    // OUT: the picture fades, the aperture closes on it, and the scan band comes
    // up as it goes -- so the last thing on screen is the band and not a shrunken
    // copy of the scene. IN: the band is already lit, holds a moment, then opens
    // while the picture comes back up underneath it.
    if (vg.tv_phase == TV_NONE) {
        vg_rast_tv(1.0f, 1.0f, 0.0f, 0.0f);
    } else if (vg.tv_phase == TV_HOLD) {
        vg_rast_tv(0.0f, 0.0f, 0.0f, 1.0f);      // dead air
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
        const float c = (vg.tv_phase == TV_OUT) ? (vg.tv_t / TV_OUT_TIME)
                                                : (1.0f - vg.tv_t / TV_IN_TIME);
        vg_rast_tv(1.0f - smoothstep(0.05f, 0.62f, c),
                   1.0f - smoothstep(0.62f, 1.00f, c),
                   // Lit for essentially the whole of it. The kill at the very end
                   // is what makes the dot go OUT rather than merely get small.
                   smoothstep(0.20f, 0.55f, c) * (1.0f - smoothstep(0.985f, 1.0f, c)),
                   smoothstep(0.00f, 0.50f, c));
    }

    vg_rast_begin_frame();

    // World, back to front. The boundary goes down before anything solid so the
    // hidden-line fills occlude it.
    vg_draw_starfield(cam);
    vg_draw_arena_grid(cam);
    vg_draw_world(cam);
    // Gated on the state, not just on ring_alive. A stale gate drawn into a match
    // would be confusing at best, and its normal is only guaranteed sane while
    // the course owns it.
    if (vg.state == VG_COURSE) vg_course_draw(cam);

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


    vg_draw_lock_box(cam);

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
    warp = floorf(warp * HUD_WARP_STEPS + 0.5f) / HUD_WARP_STEPS;

    // Instruments come up as a hologram catching: mostly absent at first,
    // flickering in, solid by the end. Driven by dropping whole frames rather
    // than by dimming, because a projector that is not holding sync loses the
    // image outright and a fade just looks like a brightness slider.
    bool draw_instruments = true;
    if (vg.hud_boot > 0.0f) {
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
    const bool  live = (vg.state != VG_PAUSE);
    const float thr  = vg.throttle_vis;

    // Strain, from what the airframe is actually doing. The same number the camera
    // shake uses, so the panel and the world agree about how hard the ship is
    // working even though they jitter on separate clocks.
    (void)thr;
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

    if (draw_instruments) {
        vg_hud_jitter(jx, jy);
        vg_hud_warp(true, warp);
        vg_draw_hud(cam, in, fps);
        vg_hud_warp(false, 1.0f);
        vg_hud_jitter(0.0f, 0.0f);
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
    if (vg.hud_boot > 0.0f) {
        const float p  = 1.0f - vg.hud_boot / HUD_BOOT_TIME;
        const int   sy = (int)(fmodf(vg.state_t * 620.0f, (float)SCR_H));
        vg_fill_rect(0, sy, SCR_W, 2, vg_dim(INK_BRIGHT, 1.0f - p));
    }

    // Everything past here stays FLAT: the steering ring has to sit exactly under
    // the finger, and the markers have to line up with what they point at. A
    // curved control that does not match where you are touching feels broken
    // rather than physical.
    vg_draw_steer_indicator(in);
    vg_draw_target_markers(cam);
    vg_draw_threat_indicator(cam);

    vg_draw_overlays();

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

    draw_fps(fps);
}
