#include "vg_render.h"
#include "vg_draw.h"
#include "vg_game.h"
#include "vg_screens.h"
#include "vg_glitch.h"
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

void vg_render_frame(const VgInput* in, float fps) {
    VgCam cam = vg_cam_make(vg.bank, vg.shake_x, vg.shake_y, vg.cam_zoom);

    vg_rast_begin_frame();

    // World, back to front. The boundary goes down before anything solid so the
    // hidden-line fills occlude it.
    vg_draw_starfield(cam);
    vg_draw_arena_grid(cam);
    vg_draw_world(cam);

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

    // Strain, from holding the throttle against the stop.
    float strain = 0.0f;
    if (live && thr > SPEED_SHAKE_AT)
        strain = (thr - SPEED_SHAKE_AT) / (1.0f - SPEED_SHAKE_AT);

    // Damage, from having just been hit. Both drive the same failure language,
    // because they are the same thing to the airframe: something is wrong with
    // the machine and the panel is where you find out.
    const float hurt = (live && vg.damage_glitch > 0.0f)
                     ? (vg.damage_glitch / DAMAGE_GLITCH) : 0.0f;

    float jx = 0.0f, jy = 0.0f;
    if (strain > 0.0f)
        vg_glitch_offset(vg.state_t, 47.0f, HUD_SHAKE_MAX * strain * strain, &jx, &jy);
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
    const float sev = (hurt * 0.55f > strain * 0.12f) ? hurt * 0.55f : strain * 0.12f;
    if (sev > 0.0f) {
        vg_glitch_patches(vg.state_t, sev);
        if (hurt > 0.35f) vg_glitch_tears(vg.state_t, hurt * 0.5f);
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

    // Last of all, over everything including the instruments.
    if (vg.state == VG_PAUSE) vg_draw_pause();

    draw_fps(fps);
}
